#pragma once

/**
 * @file dispatch_state_rules.h
 * @brief Private state, side-effect, and capacity-accounting rules.
 *
 * These predicates are shared by scheduling, preemption, and state transition
 * code. Centralizing them prevents the three paths from assigning different
 * meanings to a suspended dispatch or an occupied Agent credit.
 */

#include "master_agent/agent_dispatch/agent_dispatch.h"

namespace master_agent::agent_dispatch {
namespace {

// Suspended evidence may describe no effect, a committed checkpoint, or a
// compensated effect. UNKNOWN cannot prove a safe resumable boundary.
bool validSuspendedSideEffect(SideEffectState state) {
    switch (state) {
        case SideEffectState::NotApplicable:
        case SideEffectState::NotStarted:
        case SideEffectState::Committed:
        case SideEffectState::ConfirmedNotExecuted:
        case SideEffectState::Compensated:
            return true;
        case SideEffectState::Unknown:
            return false;
    }
    return false;
}

bool consumesAgentCredit(DispatchState state) {
    return state == DispatchState::Queued ||
           state == DispatchState::Running ||
           state == DispatchState::Unknown;
}

}  // namespace
}  // namespace master_agent::agent_dispatch

