#pragma once

/**
 * @file dag_runtime_rules.h
 * @brief Private DAG normalization, inherited-priority, and terminal-state rules.
 *
 * This header is private to Task Orchestration and is not part of the installed API.
 */

#include "master_agent/orchestrator/orchestrator.h"

#include <algorithm>
#include <cerrno>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace master_agent::orchestrator {
namespace {

IntentDAG normalizeEdges(const IntentDAG& source) {
    auto normalized = source;
    std::map<std::string, DAGNode*> by_id;
    for (auto& node : normalized.nodes) by_id[node.node_id] = &node;
    for (const auto& edge : normalized.edges) {
        if (!edge.required) continue;
        const auto target = by_id.find(edge.to_node_id);
        if (target == by_id.end()) continue;
        auto& dependencies = target->second->dependencies;
        if (std::find(dependencies.begin(), dependencies.end(),
                      edge.from_node_id) == dependencies.end()) {
            dependencies.push_back(edge.from_node_id);
        }
    }
    for (auto& node : normalized.nodes) {
        for (const auto& binding : node.result_bindings) {
            if (std::find(
                    node.dependencies.begin(),
                    node.dependencies.end(),
                    binding.source_node_id) ==
                node.dependencies.end()) {
                node.dependencies.push_back(
                    binding.source_node_id);
            }
        }
    }
    return normalized;
}

// closure priority inheritance.
std::map<std::string, TaskPriority> inheritedPriorities(
    const IntentDAG& dag) {
    std::map<std::string, const DAGNode*> nodes;
    std::map<std::string, TaskPriority> priorities;
    for (const auto& node : dag.nodes) {
        nodes[node.node_id] = &node;
        priorities[node.node_id] = node.base_priority;
    }
    std::function<void(const std::string&, TaskPriority)> inherit =
        [&](const std::string& node_id, TaskPriority priority) {
            const auto found = nodes.find(node_id);
            if (found == nodes.end()) return;
            if (isHigherPriority(priority, priorities[node_id])) {
                priorities[node_id] = priority;
            }
            for (const auto& dependency :
                 found->second->dependencies) {
                if (isHigherPriority(priority,
                                     priorities[dependency])) {
                    inherit(dependency, priority);
                }
            }
        };
    for (const auto& node : dag.nodes) {
        inherit(node.node_id, node.base_priority);
    }
    return priorities;
}

bool nodeTerminal(ActivationState state) {
    return state == ActivationState::Succeeded ||
           state == ActivationState::Failed ||
           state == ActivationState::Skipped ||
           state == ActivationState::Cancelled;
}

bool planTerminal(PlanState state) {
    return state == PlanState::Succeeded || state == PlanState::Failed ||
           state == PlanState::Cancelled || state == PlanState::Unknown;
}

}  // namespace
}  // namespace master_agent::orchestrator

