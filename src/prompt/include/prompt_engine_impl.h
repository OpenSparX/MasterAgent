#pragma once

#include "prompt_renderer.h"
#include "template_manager.h"

#include "master_agent/prompt/prompt_engine.h"

#include <memory>
#include <string>

namespace master_agent::prompt {

/// Prompt 引擎内部实现，负责模板选择、热加载和渲染。
class PromptEngineImpl : public IPromptEngine {
public:
    PromptEngineImpl(
        std::unique_ptr<ITemplateManager> template_manager,
        data_log::IDataLogService* log_service = nullptr,
        exception::IExceptionManager* exception_mgr = nullptr,
        std::string templates_json_path = "config/prompt/templates.json"
    );

    std::string buildPrompt(const PromptContext& context) override;
    std::string selectTemplate(const std::string& tpl_type) const override;
    bool reloadTemplates(const std::string& templates_path) override;

private:
    /// 模板管理器。
    std::unique_ptr<ITemplateManager> template_manager_;
    /// 预留日志依赖。
    data_log::IDataLogService* log_service_;
    /// 预留异常管理依赖。
    exception::IExceptionManager* exception_mgr_;
    /// 当前模板 JSON 文件路径。
    std::string templates_json_path_;
};

} // namespace master_agent::prompt
