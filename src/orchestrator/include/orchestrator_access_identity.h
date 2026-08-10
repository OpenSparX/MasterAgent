#pragma once

/**
 * @file orchestrator_access_identity.h
 * @brief Private Agent Service boundary and plan submission identity helpers.
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

Status validateAgentServiceCaller(const CallContext& call) {
    if (!hasHostModuleIdentity(
            call, CallerModuleId::AgentService)) {
        return Status::Error("orchestrator",
                             "ORCHESTRATOR_CALLER_NOT_ALLOWED",
                             "AgentService owns public plan submission/query");
    }
    return Status::Ok();
}

nlohmann::json dagNodeJson(const DAGNode& node);
nlohmann::json dagEdgeJson(const DAGEdge& edge);

std::string submitDigest(const OrchestratorSubmitRequest& request) {
    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& node : request.dag.nodes) {
        nodes.push_back(dagNodeJson(node));
    }
    nlohmann::json edges = nlohmann::json::array();
    for (const auto& edge : request.dag.edges) {
        edges.push_back(dagEdgeJson(edge));
    }
    nlohmann::json encoded{
        {"request_id", request.dag.request_id},
        {"nodes", nodes},
        {"edges", edges},
        {"dag_priority", toString(request.dag.priority)},
        {"dag_deadline_mono_ns", request.dag.deadline_mono_ns},
        {"schema_version", request.dag.schema_version},
        {"dag_idempotency_key", request.dag.idempotency_key},
        {"request_idempotency_key", request.idempotency_key},
        {"expected_capability_digest",
         request.expected_capability_digest},
        {"trace_id", request.trace_id},
        {"admission",
         {{"principal_id_hash",
           request.admission.principal_id_hash},
          {"source_type", request.admission.source_type},
          {"granted_priority",
           toString(request.admission.granted_priority)},
          {"p0_authorization",
           request.admission.p0_authorization},
          {"p0_allowed_capabilities",
           request.admission.p0_allowed_capabilities},
          {"policy_snapshot_id",
           request.admission.policy_snapshot_id},
          {"policy_digest", request.admission.policy_digest},
          {"authorization_ref",
           request.admission.authorization_ref},
          {"max_nodes", request.admission.max_nodes},
          {"allowed_capabilities",
           request.admission.allowed_capabilities},
          {"granted_permissions",
           request.admission.granted_permissions},
          {"retry_policy_digest",
           request.admission.retry_policy_digest},
          {"retry_policies_digest",
           retryPoliciesDigest(
               request.admission.retry_policies)},
          {"deadline_mono_ns",
           request.admission.deadline_mono_ns}}}};
    return secureDigest(encoded.dump());
}

}  // namespace
}  // namespace master_agent::orchestrator

