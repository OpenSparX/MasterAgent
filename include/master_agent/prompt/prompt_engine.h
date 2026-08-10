#pragma once

/**
 * @file prompt_engine.h
 * @brief Prompt 引擎对外接口
 *
 * 这一层只暴露 PromptContext、IPromptEngine 和 createPromptEngine(...)，
 * 让调用方只依赖稳定抽象，不感知内部模板管理、槽位提取和渲染细节。
 */

#include <memory>
#include <string>

namespace data_log {
class IDataLogService;
}

namespace exception {
class IExceptionManager;
}

namespace master_agent::prompt {

/// Prompt 模块标准输入。
/// 其中 tpl_type 用于选择模板，其他字段作为槽位参与渲染。
struct PromptContext {
    std::string tpl_type = "default";
    std::string context;
    std::string memory_context;
    std::string emotion_tag;
    std::string skill_candidates;
    std::string vehicle_state; 
};

/// Prompt 模块对外抽象接口。
class IPromptEngine {
public:
    virtual ~IPromptEngine() = default;

    /// 构建最终 Prompt。
    /// @param context 上层传入的完整上下文。
    /// @return 渲染后的最终提示词字符串。
    virtual std::string buildPrompt(const PromptContext& context) = 0;

    /// 根据模板标识选择模板。
    /// 若未命中则回退到默认模板。
    /// @param tpl_type 模板标识。
    /// @return 模板文本。
    virtual std::string selectTemplate(const std::string& tpl_type) const = 0;

    /// 从单个 JSON 文件重载全部模板。
    /// @param templates_path 模板 JSON 文件路径。
    /// @return 是否重载成功。
    virtual bool reloadTemplates(const std::string& templates_path) = 0;
};

/// 创建 Prompt 引擎实例。
/// 这里保留日志和异常管理依赖注入入口，便于后续接入完整工程。
std::unique_ptr<IPromptEngine> createPromptEngine(
    data_log::IDataLogService* log_service = nullptr,
    exception::IExceptionManager* exception_mgr = nullptr
);

} // namespace master_agent::prompt
