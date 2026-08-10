/**
 * @file exception_digest.cpp
 * @brief Computes deterministic exception batch checksums.
 */

#include "include/exception_validation.h"
#include "include/exception_identity.h"
#include "include/exception_durability.h"
#include "include/exception_journal_codec.h"
#include "include/exception_journal_codec.h"
#include "include/exception_runtime_policy.h"

namespace master_agent::exception {

std::string exceptionBatchChecksum(
    const ExceptionReportRequest& request) {
    Json encoded{
        {"report_id", request.report_id},
        {"requested_durability",
         static_cast<std::uint8_t>(
             request.requested_durability)},
        {"source_redaction_proof",
         request.source_redaction_proof}};
    encoded["occurrences"] = Json::array();
    for (const auto& occurrence : request.occurrences) {
        encoded["occurrences"].push_back(
            occurrenceDigest(occurrence));
    }
    return secureDigest(encoded.dump());
}


}  // namespace master_agent::exception
