#pragma once

/**
 * @file trace_filter.h
 * @brief Private trace-query matching helper.
 *
 * This header is private to Data Log and is not part of the installed API.
 */

#include "master_agent/data_log/data_log_service.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif


namespace master_agent::data_log {
namespace {

bool matches(const LogEvent& event, const TraceQuery& query) {
    if (query.trace_id && event.context.trace_id != *query.trace_id) {
        return false;
    }
    if (query.request_id && event.context.request_id != *query.request_id) {
        return false;
    }
    if (query.plan_id &&
        (!event.context.plan_id || *event.context.plan_id != *query.plan_id)) {
        return false;
    }
    if (query.execution_id &&
        (!event.context.execution_id ||
         *event.context.execution_id != *query.execution_id)) {
        return false;
    }
    return true;
}

}  // namespace
}  // namespace master_agent::data_log

