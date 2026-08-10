/**
 * @file service_lifecycle.cpp
 * @brief Owns exception-journal initialization, recovery, and persistence.
 */

#include "include/exception_validation.h"
#include "include/exception_identity.h"
#include "include/exception_durability.h"
#include "include/exception_journal_codec.h"
#include "include/exception_journal_codec.h"
#include "include/exception_runtime_policy.h"

namespace master_agent::exception {

ExceptionManager::ExceptionManager(
    std::shared_ptr<IRuntimeClock> clock, std::shared_ptr<IdGenerator> ids,
    std::shared_ptr<data_log::IDataLogService> log)
    : ExceptionManager(compatibilityStorage(ids), std::move(clock),
                          std::move(ids), std::move(log)) {}

ExceptionManager::ExceptionManager(
    std::filesystem::path storage_directory,
    std::shared_ptr<IRuntimeClock> clock, std::shared_ptr<IdGenerator> ids,
    std::shared_ptr<data_log::IDataLogService> log,
    DurabilitySync durability_sync, bool auto_initialize)
    : storage_directory_(std::move(storage_directory)),
      clock_(std::move(clock)),
      ids_(std::move(ids)),
      log_(std::move(log)),
      durability_sync_(durability_sync ? std::move(durability_sync)
                                      : nativeDurabilitySync) {
    if (auto_initialize) {
        initialization_status_ = initialize();
    }
}

ExceptionManager::~ExceptionManager() {
    if (journal_stream_.is_open()) journal_stream_.close();
    releaseWriterLease();
}

Status ExceptionManager::acquireWriterLeaseUnlocked() {
    if (writer_lease_handle_ != -1) return Status::Ok();
    const auto path =
        storage_directory_ / "writer.lock";
#ifdef _WIN32
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Status::Error(
            "exception", "EXM_WRITER_LEASE_HELD",
            "another process owns the exception journal directory", true);
    }
    writer_lease_handle_ =
        reinterpret_cast<std::intptr_t>(handle);
#else
    const int descriptor =
        ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (descriptor < 0 ||
        ::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        if (descriptor >= 0) ::close(descriptor);
        return Status::Error(
            "exception", "EXM_WRITER_LEASE_HELD",
            "another process owns the exception journal directory", true);
    }
    writer_lease_handle_ =
        static_cast<std::intptr_t>(descriptor);
#endif
    return Status::Ok();
}

void ExceptionManager::releaseWriterLease() noexcept {
    if (writer_lease_handle_ == -1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(
        writer_lease_handle_));
#else
    const int descriptor =
        static_cast<int>(writer_lease_handle_);
    (void)::flock(descriptor, LOCK_UN);
    (void)::close(descriptor);
#endif
    writer_lease_handle_ = -1;
}

Status ExceptionManager::initialize() {

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (ready_) return Status::Ok();
        if (journal_commit_inflight_) {
            return Status::Error(
                "exception", "EXM_WRITER_BUSY",
                "exception journal initialization is already in progress",
                true);
        }
        if (!clock_ || !ids_ || storage_directory_.empty()) {
            initialization_status_ = Status::Error(
                "exception", "EXM_CONFIGURATION_INVALID",
                "clock, ids and an independent storage directory are required");
            return initialization_status_;
        }
        journal_commit_inflight_ = true;
        try {
            if (journal_stream_.is_open()) {
                journal_stream_.close();
            }
            journal_stream_.clear();
            const auto active_directory =
                storage_directory_ / "journal" / "active";
            std::filesystem::create_directories(active_directory);
            std::filesystem::create_directories(
                storage_directory_ / "journal" / "sealed");
            std::filesystem::create_directories(
                storage_directory_ / "snapshots");
            std::filesystem::create_directories(
                storage_directory_ / "indexes");
            std::filesystem::create_directories(
                storage_directory_ / "outbox");
            std::filesystem::create_directories(
                storage_directory_ / "emergency");
            const auto writer_lease =
                acquireWriterLeaseUnlocked();
            if (!writer_lease.ok) {
                initialization_status_ = writer_lease;
                journal_commit_inflight_ = false;
                return initialization_status_;
            }
            active_journal_path_ =
                active_directory / "exception.jsonl";
            const auto recovered = recoverJournalUnlocked();
            if (!recovered.ok) {
                initialization_status_ = recovered;
                journal_commit_inflight_ = false;
                return initialization_status_;
            }
            journal_stream_.open(active_journal_path_,
                                 std::ios::binary | std::ios::app);
            if (!journal_stream_.is_open()) {
                initialization_status_ = Status::Error(
                    "exception", "EXM_DURABILITY_UNAVAILABLE",
                    "failed to open independent exception journal", true);
                journal_commit_inflight_ = false;
                return initialization_status_;
            }
        } catch (...) {
            initialization_status_ = Status::Error(
                "exception", "EXM_DURABILITY_UNAVAILABLE",
                "failed to initialize independent exception journal", true);
            journal_commit_inflight_ = false;
            return initialization_status_;
        }
    }

    // External durability callbacks run with neither the exception state
    // mutex nor a published Ready state.  Re-entrant getException therefore
    // returns EXM_NOT_INITIALIZED instead of deadlocking.
    Status synced;
    try {
        std::lock_guard<std::mutex> writer_lock(journal_writer_mutex_);
        synced = durability_sync_(active_journal_path_);
    } catch (...) {
        synced = durabilityUnknown(
            "exception journal recovery sync threw");
    }
    std::uint64_t recovered_writer_epoch = 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        journal_commit_inflight_ = false;
        if (!synced.ok) {
            initialization_status_ = durabilityUnknown(
                "exception journal recovery sync failed");
            return initialization_status_;
        }
        ready_ = true;
        journal_fenced_ = false;
        initialization_status_ = Status::Ok();
        recovered_writer_epoch = writer_epoch_;
    }
    CallContext recovery_call{
        CallerModuleId::ExceptionManager, "exception-recovery",
        "exception-recovery", "system", TaskPriority::P1,
        clock_->monotonicNowNs() + 5'000'000'000LL,
        "ExceptionManager", recovered_writer_epoch,
        "internal-observability"};
    drainPendingObservations(recovery_call);
    return Status::Ok();
}

Status ExceptionManager::recoverJournalUnlocked() {

    groups_.clear();
    fingerprint_to_exception_.clear();
    occurrence_results_.clear();
    report_results_.clear();
    mutation_results_.clear();
    occurrence_digests_.clear();
    report_digests_.clear();
    mutation_digests_.clear();
    producer_sequence_watermarks_.clear();
    producer_sequence_owners_.clear();
    pending_observations_.clear();
    fenced_transaction_digests_.clear();
    fenced_transaction_failures_.clear();
    journal_sequence_ = 0;
    observation_sequence_ = 0;
    writer_epoch_ = 0;

    if (!std::filesystem::exists(active_journal_path_)) {
        std::ofstream create(active_journal_path_,
                             std::ios::binary | std::ios::app);
        if (!create.is_open()) {
            return Status::Error(
                "exception", "EXM_DURABILITY_UNAVAILABLE",
                "cannot create exception journal", true);
        }
        writer_epoch_ = 1;
        return Status::Ok();
    }

    std::ifstream input(active_journal_path_, std::ios::binary);
    if (!input.is_open()) {
        return Status::Error("exception", "EXM_DURABILITY_UNAVAILABLE",
                             "cannot read exception journal", true);
    }
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    std::size_t offset = 0;
    std::size_t valid_bytes = 0;
    std::map<std::string, std::string> recovered_transactions;
    std::uint64_t expected_journal_sequence = 1;
    while (offset < bytes.size()) {
        const auto newline = bytes.find('\n', offset);
        const bool complete_line = newline != std::string::npos;
        const auto end = complete_line ? newline : bytes.size();
        const auto line = bytes.substr(offset, end - offset);

        if (!complete_line) {
            std::filesystem::resize_file(active_journal_path_,
                                         valid_bytes);
            break;
        }
        if (line.empty()) {
            return Status::Error(
                "exception", "EXM_JOURNAL_INTEGRITY",
                "empty committed exception journal frame");
        }
        try {
            const auto frame = Json::parse(line);
            if (frame.at("schema_version").get<std::uint32_t>() !=
                kJournalSchemaVersion) {
                throw std::runtime_error("schema");
            }
            const auto payload = frame.at("payload");
            const auto payload_bytes = payload.dump();
            if (frame.at("frame_length").get<std::uint64_t>() !=
                    payload_bytes.size() ||
                frame.at("payload_hash").get<std::string>() !=
                    secureDigest(payload_bytes) ||
                frame.at("crc32").get<std::uint32_t>() !=
                    crc32(payload_bytes)) {
                throw std::runtime_error("frame integrity");
            }
            const auto transaction_id =
                payload.at("transaction_id").get<std::string>();
            const auto request_digest =
                payload.at("request_digest").get<std::string>();
            const auto sequence =
                payload.at("journal_sequence").get<std::uint64_t>();
            if (sequence != expected_journal_sequence) {
                return Status::Error(
                    "exception", "EXM_JOURNAL_INTEGRITY",
                    "exception journal sequence is not strictly continuous");
            }
            ++expected_journal_sequence;
            const auto canonical_payload_digest =
                secureDigest(payload_bytes);
            const auto previous =
                recovered_transactions.find(transaction_id);
            if (previous != recovered_transactions.end()) {
                if (previous->second != canonical_payload_digest) {
                    return Status::Error(
                        "exception", "EXM_JOURNAL_INTEGRITY",
                        "transaction id has a different canonical payload");
                }
                valid_bytes = end + 1;
                offset = valid_bytes;
                continue;
            }
            recovered_transactions[transaction_id] =
                canonical_payload_digest;
            journal_sequence_ = sequence;
            writer_epoch_ = std::max(
                writer_epoch_,
                payload.at("writer_epoch").get<std::uint64_t>());
            const auto frame_durability =
                static_cast<data_log::DurabilityClass>(
                    payload.at("durability").get<std::uint8_t>());
            if (!isD2OrD3(frame_durability)) {
                return Status::Error(
                    "exception", "EXM_JOURNAL_INTEGRITY",
                    "exception transaction has invalid durability");
            }

            const auto kind = payload.at("kind").get<std::string>();
            if (kind == "report") {
                for (const auto& encoded_group :
                     payload.at("groups_after")) {
                    auto group = groupFromJson(encoded_group);
                    if (!validGroupState(group)) {
                        return Status::Error(
                            "exception", "EXM_JOURNAL_INTEGRITY",
                            "recovered exception group has invalid schema");
                    }
                    fingerprint_to_exception_[group.fingerprint] =
                        group.exception_id;
                    groups_[group.exception_id] = std::move(group);
                }
                for (const auto& entry :
                     payload.at("occurrence_entries")) {
                    const auto key =
                        entry.at("occurrence_key").get<std::string>();
                    const auto accepted =
                        acceptedFromJson(entry.at("accepted"));
                    if (!validAcceptedState(accepted)) {
                        return Status::Error(
                            "exception", "EXM_JOURNAL_INTEGRITY",
                            "recovered occurrence result has invalid schema");
                    }
                    occurrence_results_[key] = accepted;
                    occurrence_digests_[key] =
                        entry.at("digest").get<std::string>();
                    const auto endpoint =
                        entry.at("producer_endpoint_id")
                            .get<std::string>();
                    const auto epoch =
                        entry.at("producer_epoch")
                            .get<std::uint64_t>();
                    const auto sequence =
                        entry.at("effective_sequence")
                            .get<std::uint64_t>();
                    const auto producer = producerKey(endpoint, epoch);
                    if (endpoint.empty() || epoch == 0 ||
                        sequence == 0 ||
                        sequence <=
                            producer_sequence_watermarks_[producer]) {
                        return Status::Error(
                            "exception", "EXM_JOURNAL_INTEGRITY",
                            "recovered producer sequence is invalid");
                    }
                    producer_sequence_watermarks_[producer] =
                        sequence;
                    producer_sequence_owners_[sequenceKey(
                        endpoint, epoch, sequence)] = key;
                }
                auto result =
                    reportResultFromJson(payload.at("result"));
                if (result.report_id.empty() ||
                    result.results.size() !=
                        static_cast<std::size_t>(
                            result.accepted_count +
                            result.rejected_count) ||
                    std::any_of(
                        result.results.begin(),
                        result.results.end(),
                        [](const ExceptionAccepted& accepted) {
                            return accepted.disposition !=
                                       ExceptionDisposition::Rejected &&
                                   !validAcceptedState(accepted);
                        })) {
                    return Status::Error(
                        "exception", "EXM_JOURNAL_INTEGRITY",
                        "recovered report result has invalid schema");
                }
                report_digests_[result.report_id] = request_digest;
                report_results_[result.report_id] =
                    std::move(result);
            } else if (kind == "mutation") {
                auto group = groupFromJson(payload.at("group_after"));
                if (!validGroupState(group)) {
                    return Status::Error(
                        "exception", "EXM_JOURNAL_INTEGRITY",
                        "recovered mutation group has invalid schema");
                }
                groups_[group.exception_id] = group;
                fingerprint_to_exception_[group.fingerprint] =
                    group.exception_id;
                ExceptionMutationResult result;
                result.changed =
                    payload.at("changed").get<bool>();
                result.group = std::move(group);
                const auto mutation_id =
                    payload.at("idempotency_key").get<std::string>();
                mutation_digests_[mutation_id] = request_digest;
                mutation_results_[mutation_id] =
                    std::move(result);
            } else if (kind == "observation_ack") {
                const auto source_transaction_id =
                    payload.at("source_transaction_id")
                        .get<std::string>();
                const auto batch_id =
                    payload.at("batch_id").get<std::string>();
                const auto batch_digest =
                    payload.at("batch_digest").get<std::string>();
                const auto pending = std::find_if(
                    pending_observations_.begin(),
                    pending_observations_.end(),
                    [&](const PendingObservation& item) {
                        return item.source_transaction_id ==
                                   source_transaction_id &&
                               item.batch.batch_id == batch_id;
                    });
                if (pending == pending_observations_.end() ||
                    pending->batch_digest != batch_digest ||
                    request_digest != batch_digest ||
                    pending->durability != frame_durability) {
                    return Status::Error(
                        "exception", "EXM_JOURNAL_INTEGRITY",
                        "observation ACK does not bind its source batch");
                }
                pending_observations_.erase(pending);
            } else {
                return Status::Error(
                    "exception", "EXM_JOURNAL_INTEGRITY",
                    "unknown exception transaction kind");
            }
            if (payload.contains("observation") &&
                !payload.at("observation").is_null()) {
                auto batch =
                    batchFromJson(payload.at("observation"));
                if (!validObservationBatch(batch)) {
                    return Status::Error(
                        "exception", "EXM_JOURNAL_INTEGRITY",
                        "recovered observation outbox has invalid schema");
                }
                observation_sequence_ =
                    std::max(observation_sequence_,
                             batch.last_sequence);
                PendingObservation pending;
                pending.batch_digest =
                    secureDigest(batchToJson(batch).dump());
                pending.source_transaction_id = transaction_id;
                pending.durability = frame_durability;
                pending.batch = std::move(batch);
                pending_observations_.push_back(
                    std::move(pending));
            }
            valid_bytes = end + 1;
            offset = valid_bytes;
        } catch (const std::exception&) {
            // A newline is the physical commit marker.  Therefore any
            // schema/hash/CRC/JSON error in a newline-terminated frame is
            // committed corruption, including the last frame.
            return Status::Error(
                "exception", "EXM_JOURNAL_INTEGRITY",
                "newline-terminated exception journal frame is corrupt");
        }
    }
    ++writer_epoch_;
    if (writer_epoch_ == 0) writer_epoch_ = 1;
    return Status::Ok();
}

Status ExceptionManager::persistTransactionUnlocked(
    const std::string& transaction_payload,
    data_log::DurabilityClass durability) {
    Json payload;
    try {
        payload = Json::parse(transaction_payload);
    } catch (...) {
        return Status::Error(
            "exception", "EXM_JOURNAL_INTEGRITY",
            "prepared exception transaction is not canonical JSON");
    }
    const Json frame{
        {"schema_version", kJournalSchemaVersion},
        {"frame_length", transaction_payload.size()},
        {"payload_hash", secureDigest(transaction_payload)},
        {"crc32", crc32(transaction_payload)},
        {"payload", payload}};
    std::lock_guard<std::mutex> writer_lock(journal_writer_mutex_);
    if (!journal_stream_.is_open()) {
        return Status::Error(
            "exception", "EXM_DURABILITY_UNAVAILABLE",
            "independent exception journal is not open", true);
    }
    Status persisted = Status::Ok();
    try {
        journal_stream_ << frame.dump() << '\n';
        journal_stream_.flush();
    } catch (...) {
        persisted = durabilityUnknown(
            "exception journal write threw after admission");
    }
    if (persisted.ok && !journal_stream_.good()) {
        persisted = durabilityUnknown(
            "exception journal write result is ambiguous");
    } else if (persisted.ok &&
               durability ==
                   data_log::DurabilityClass::D3Fsynced) {
        try {
            const auto synced = durability_sync_(active_journal_path_);
            if (!synced.ok) {
                persisted = durabilityUnknown(
                    "exception journal fsync result is ambiguous");
            }
        } catch (...) {
            persisted = durabilityUnknown(
                "exception journal fsync threw after frame write");
        }
    }
    return persisted;
}

// Reporting deduplicates both the batch and each producer-scoped occurrence.
// The authoritative exception transaction is durable before its log projection.

}  // namespace master_agent::exception
