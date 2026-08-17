/**
 * @file sparx_agent_scheduler.cpp
 * @brief Agent Scheduler — OS-level process management implementation.
 */

#include "sparx_agent_scheduler.h"

#include <algorithm>
#include <chrono>

namespace sparx::os {

// ─── AgentScheduler ─────────────────────────────────────────────────────────

AgentScheduler::AgentScheduler(SchedulerConfig config)
    : config_(std::move(config)) {}

AgentScheduler::~AgentScheduler() {
    stop();
}

void AgentScheduler::start() {
    if (running_.load()) return;
    running_.store(true);
    tick_thread_ = std::thread(&AgentScheduler::tickLoop, this);
}

void AgentScheduler::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (tick_thread_.joinable()) tick_thread_.join();
}

AgentId AgentScheduler::spawn(const std::string& name, SchedClass sched_class,
                              int priority, const std::string& agent_type) {
    std::lock_guard<std::mutex> lock(mutex_);

    AgentPCB pcb;
    pcb.id = next_id_++;
    pcb.name = name;
    pcb.agent_type = agent_type;
    pcb.sched_class = sched_class;
    pcb.priority = priority;
    pcb.state = AgentState::Ready;
    pcb.created_at = std::chrono::steady_clock::now();
    pcb.last_scheduled = pcb.created_at;
    pcb.resources.tokens_budget = config_.default_token_budget;

    // Set time slice based on class
    switch (sched_class) {
        case SchedClass::RealTime:
            pcb.time_slice_remaining = 1;  // RT always runs to completion
            break;
        case SchedClass::Interactive:
            pcb.time_slice_remaining = config_.interactive_time_slice;
            break;
        case SchedClass::Batch:
            pcb.time_slice_remaining = config_.batch_time_slice;
            break;
        case SchedClass::Idle:
            pcb.time_slice_remaining = 1;
            break;
    }

    AgentId id = pcb.id;
    agents_[id] = std::move(pcb);
    enqueue(id);

    stats_.total++;
    stats_.ready++;
    emit(SchedulerEvent::Type::AgentSpawned, id, name);
    return id;
}

bool AgentScheduler::suspend(AgentId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) return false;
    auto& pcb = it->second;

    if (pcb.state != AgentState::Running && pcb.state != AgentState::Ready)
        return false;

    if (pcb.state == AgentState::Running) {
        running_set_.erase(id);
        stats_.running--;
    } else {
        dequeue(id);
        stats_.ready--;
    }

    pcb.state = AgentState::Suspended;
    stats_.suspended++;
    emit(SchedulerEvent::Type::AgentSuspended, id);
    return true;
}

bool AgentScheduler::resume(AgentId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) return false;
    auto& pcb = it->second;

    if (pcb.state != AgentState::Suspended) return false;

    pcb.state = AgentState::Ready;
    pcb.wait_ticks = 0;
    stats_.suspended--;
    stats_.ready++;
    enqueue(id);
    emit(SchedulerEvent::Type::AgentResumed, id);
    return true;
}

bool AgentScheduler::kill(AgentId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) return false;
    auto& pcb = it->second;

    // Remove from whatever state it's in
    switch (pcb.state) {
        case AgentState::Running:
            running_set_.erase(id);
            stats_.running--;
            break;
        case AgentState::Ready:
            dequeue(id);
            stats_.ready--;
            break;
        case AgentState::Suspended:
            stats_.suspended--;
            break;
        case AgentState::Blocked:
            stats_.blocked--;
            break;
        default:
            break;
    }

    pcb.state = AgentState::Terminated;
    stats_.terminated++;

    // Unblock agents waiting on this one
    for (AgentId waiter : pcb.blocked_by_me) {
        auto wit = agents_.find(waiter);
        if (wit != agents_.end()) {
            wit->second.waiting_on.erase(id);
            if (wit->second.waiting_on.empty() &&
                wit->second.state == AgentState::Blocked) {
                wit->second.state = AgentState::Ready;
                wit->second.block_reason = BlockReason::None;
                stats_.blocked--;
                stats_.ready++;
                enqueue(waiter);
            }
        }
    }
    pcb.blocked_by_me.clear();

    emit(SchedulerEvent::Type::AgentTerminated, id);
    return true;
}

bool AgentScheduler::block(AgentId id, BlockReason reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) return false;
    auto& pcb = it->second;

    if (pcb.state != AgentState::Running) return false;

    running_set_.erase(id);
    stats_.running--;
    pcb.state = AgentState::Blocked;
    pcb.block_reason = reason;
    pcb.last_blocked = std::chrono::steady_clock::now();
    stats_.blocked++;
    emit(SchedulerEvent::Type::AgentBlocked, id);
    return true;
}

bool AgentScheduler::unblock(AgentId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) return false;
    auto& pcb = it->second;

    if (pcb.state != AgentState::Blocked) return false;

    pcb.state = AgentState::Ready;
    pcb.block_reason = BlockReason::None;
    pcb.wait_ticks = 0;
    stats_.blocked--;
    stats_.ready++;
    enqueue(id);
    emit(SchedulerEvent::Type::AgentUnblocked, id);
    return true;
}

std::optional<AgentId> AgentScheduler::schedule() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if preemption needed
    if (config_.enable_preemption && !running_set_.empty()) {
        auto preempt_id = checkPreemption();
        if (preempt_id) {
            // Preempt the lowest priority running agent
            AgentId victim = *running_set_.rbegin();  // last = lowest priority
            auto vit = agents_.find(victim);
            if (vit != agents_.end()) {
                vit->second.state = AgentState::Ready;
                running_set_.erase(victim);
                stats_.running--;
                stats_.ready++;
                enqueue(victim);
                stats_.preemption_count++;
                emit(SchedulerEvent::Type::AgentPreempted, victim);
            }
        }
    }

    // Check concurrency limit
    if (running_set_.size() >= config_.max_concurrent) {
        return std::nullopt;
    }

    auto next = pickNext();
    if (!next) return std::nullopt;

    AgentId id = *next;
    auto& pcb = agents_[id];
    pcb.state = AgentState::Running;
    pcb.last_scheduled = std::chrono::steady_clock::now();
    running_set_.insert(id);
    stats_.running++;
    stats_.ready--;
    stats_.schedule_count++;
    emit(SchedulerEvent::Type::AgentScheduled, id);
    return id;
}

bool AgentScheduler::yield(AgentId id, uint32_t tokens_used) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) return false;
    auto& pcb = it->second;

    if (pcb.state != AgentState::Running) return false;

    // Update accounting
    pcb.resources.tokens_consumed += tokens_used;
    pcb.resources.turns_completed++;
    updateVRuntime(pcb, tokens_used);
    stats_.total_tokens += tokens_used;

    // Check budget
    if (!pcb.resources.withinBudget()) {
        running_set_.erase(id);
        stats_.running--;
        pcb.state = AgentState::Blocked;
        pcb.block_reason = BlockReason::ResourceLimit;
        stats_.blocked++;
        emit(SchedulerEvent::Type::BudgetExhausted, id);
        return false;
    }

    // Check time slice
    if (pcb.time_slice_remaining > 0) {
        pcb.time_slice_remaining--;
    }
    if (pcb.time_slice_remaining == 0) {
        // Time slice expired — back to ready queue
        running_set_.erase(id);
        stats_.running--;
        pcb.state = AgentState::Ready;

        // Reset time slice
        switch (pcb.sched_class) {
            case SchedClass::Interactive:
                pcb.time_slice_remaining = config_.interactive_time_slice;
                break;
            case SchedClass::Batch:
                pcb.time_slice_remaining = config_.batch_time_slice;
                break;
            default:
                pcb.time_slice_remaining = 1;
                break;
        }

        stats_.ready++;
        enqueue(id);
        return false;  // Agent must yield
    }

    return true;  // Agent keeps running
}

void AgentScheduler::terminate(AgentId id, int /*exit_code*/) {
    kill(id);
}

std::optional<AgentPCB> AgentScheduler::getAgent(AgentId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) return std::nullopt;
    return it->second;
}

std::vector<AgentPCB> AgentScheduler::agentsByState(AgentState state) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AgentPCB> result;
    for (const auto& [_, pcb] : agents_) {
        if (pcb.state == state) result.push_back(pcb);
    }
    return result;
}

std::vector<AgentPCB> AgentScheduler::allAgents() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AgentPCB> result;
    result.reserve(agents_.size());
    for (const auto& [_, pcb] : agents_) {
        result.push_back(pcb);
    }
    return result;
}

AgentScheduler::Stats AgentScheduler::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void AgentScheduler::addDependency(AgentId waiter, AgentId dependency) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto wit = agents_.find(waiter);
    auto dit = agents_.find(dependency);
    if (wit == agents_.end() || dit == agents_.end()) return;

    wit->second.waiting_on.insert(dependency);
    dit->second.blocked_by_me.insert(waiter);
}

void AgentScheduler::removeDependency(AgentId waiter, AgentId dependency) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto wit = agents_.find(waiter);
    auto dit = agents_.find(dependency);

    if (wit != agents_.end()) wit->second.waiting_on.erase(dependency);
    if (dit != agents_.end()) dit->second.blocked_by_me.erase(waiter);
}

void AgentScheduler::onEvent(SchedulerCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back(std::move(cb));
}

void AgentScheduler::setTokenBudget(AgentId id, uint64_t budget) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agents_.find(id);
    if (it != agents_.end()) {
        it->second.resources.tokens_budget = budget;
    }
}

uint64_t AgentScheduler::remainingBudget(AgentId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) return 0;
    auto& r = it->second.resources;
    if (r.tokens_budget == 0) return UINT64_MAX;  // unlimited
    return r.tokens_budget > r.tokens_consumed
        ? r.tokens_budget - r.tokens_consumed : 0;
}

// ─── Internal methods ───────────────────────────────────────────────────────

std::optional<AgentId> AgentScheduler::pickNext() {
    // Priority order: RT > Interactive > Batch > Idle
    // Within each class: sort by effective priority (includes aging boost)

    auto pickFromQueue = [&](std::deque<AgentId>& queue) -> std::optional<AgentId> {
        if (queue.empty()) return std::nullopt;

        // Find highest effective priority in queue
        AgentId best = queue.front();
        int best_prio = agents_[best].effectivePriority();

        for (AgentId id : queue) {
            int prio = agents_[id].effectivePriority();
            if (prio > best_prio) {
                best = id;
                best_prio = prio;
            }
            // CFS tiebreaker: lower vruntime wins (more fair share owed)
            if (prio == best_prio && agents_[id].vruntime < agents_[best].vruntime) {
                best = id;
            }
        }

        // Remove from queue
        queue.erase(std::remove(queue.begin(), queue.end(), best), queue.end());
        return best;
    };

    if (auto id = pickFromQueue(rt_queue_)) return id;
    if (auto id = pickFromQueue(interactive_queue_)) return id;
    if (auto id = pickFromQueue(batch_queue_)) return id;
    if (auto id = pickFromQueue(idle_queue_)) return id;
    return std::nullopt;
}

std::optional<AgentId> AgentScheduler::checkPreemption() {
    // Check if there's a higher-priority agent waiting than the lowest running
    if (running_set_.empty()) return std::nullopt;

    // Find lowest priority among running agents
    int lowest_running = INT32_MAX;
    SchedClass lowest_class = SchedClass::Idle;
    for (AgentId rid : running_set_) {
        auto& pcb = agents_[rid];
        if (static_cast<uint8_t>(pcb.sched_class) > static_cast<uint8_t>(lowest_class)) {
            lowest_class = pcb.sched_class;
        }
        lowest_running = std::min(lowest_running, pcb.effectivePriority());
    }

    // Check if any waiting agent has higher class
    if (!rt_queue_.empty() && lowest_class > SchedClass::RealTime)
        return rt_queue_.front();
    if (!interactive_queue_.empty() && lowest_class > SchedClass::Interactive)
        return interactive_queue_.front();

    return std::nullopt;
}

void AgentScheduler::tickLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.tick_interval_ms));
        if (!running_.load()) break;

        std::lock_guard<std::mutex> lock(mutex_);

        // Aging: increment wait_ticks for all Ready agents
        auto ageQueue = [&](std::deque<AgentId>& queue) {
            for (AgentId id : queue) {
                auto it = agents_.find(id);
                if (it != agents_.end()) {
                    it->second.wait_ticks++;
                    if (it->second.wait_ticks > 0 &&
                        it->second.wait_ticks % AgentPCB::kAgingThreshold == 0) {
                        stats_.aging_boosts++;
                        emit(SchedulerEvent::Type::AgingBoost, id);
                    }
                }
            }
        };
        ageQueue(batch_queue_);
        ageQueue(idle_queue_);

        // Update CPU time for running agents
        for (AgentId id : running_set_) {
            auto it = agents_.find(id);
            if (it != agents_.end()) {
                it->second.resources.cpu_time +=
                    std::chrono::milliseconds(config_.tick_interval_ms);
            }
        }
    }
}

void AgentScheduler::enqueue(AgentId id) {
    auto it = agents_.find(id);
    if (it == agents_.end()) return;

    switch (it->second.sched_class) {
        case SchedClass::RealTime:    rt_queue_.push_back(id); break;
        case SchedClass::Interactive: interactive_queue_.push_back(id); break;
        case SchedClass::Batch:       batch_queue_.push_back(id); break;
        case SchedClass::Idle:        idle_queue_.push_back(id); break;
    }
}

void AgentScheduler::dequeue(AgentId id) {
    auto removeFrom = [&](std::deque<AgentId>& q) {
        q.erase(std::remove(q.begin(), q.end(), id), q.end());
    };
    removeFrom(rt_queue_);
    removeFrom(interactive_queue_);
    removeFrom(batch_queue_);
    removeFrom(idle_queue_);
}

void AgentScheduler::emit(SchedulerEvent::Type type, AgentId id,
                          const std::string& detail) {
    SchedulerEvent event{type, id, detail, std::chrono::steady_clock::now()};
    for (const auto& cb : callbacks_) {
        cb(event);
    }
}

void AgentScheduler::updateVRuntime(AgentPCB& pcb, uint32_t tokens) {
    // CFS virtual runtime: vruntime += tokens / weight
    // Higher nice = higher weight divisor = slower vruntime growth = less CPU share
    float weight = 1.0f + static_cast<float>(pcb.nice) * 0.1f;
    pcb.vruntime += static_cast<uint64_t>(tokens / weight);
}

}  // namespace sparx::os
