/**
 * @file module_factory_bridge.cpp
 * @brief Bridges reserved dependency-injection declarations across namespaces.
 *
 * The supplied Prompt and Skill headers forward-declare their reserved logging
 * dependencies in the global namespace. Existing Master Agent headers define
 * those services under master_agent. Both reference implementations currently
 * ignore the injected pointers, so this bridge exposes the include-order form
 * expected by existing callers and delegates to the supplied null-dependency
 * factories without changing either module-owner file.
 */

// These two headers must precede the concrete Master Agent service headers.
#include "master_agent/prompt/prompt_engine.h"
#include "master_agent/skill/skill_engine.h"

#include "master_agent/data_log/data_log_service.h"
#include "master_agent/exception/exception_manager.h"

namespace master_agent::prompt {

std::unique_ptr<IPromptEngine> createPromptEngine(
    master_agent::data_log::IDataLogService*,
    master_agent::exception::IExceptionManager*) {
    return createPromptEngine(
        static_cast<::data_log::IDataLogService*>(nullptr),
        static_cast<::exception::IExceptionManager*>(nullptr));
}

}  // namespace master_agent::prompt

namespace master_agent::skill {

std::unique_ptr<ISkillEngine> createSkillEngine(
    master_agent::data_log::IDataLogService*,
    master_agent::exception::IExceptionManager*) {
    return createSkillEngine(
        static_cast<::data_log::IDataLogService*>(nullptr),
        static_cast<::exception::IExceptionManager*>(nullptr));
}

}  // namespace master_agent::skill
