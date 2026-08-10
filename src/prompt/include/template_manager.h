#pragma once

#include "prompt_template_types.h"

#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace master_agent::prompt {

/// 模板管理抽象接口，只负责热加载和检索。
class ITemplateManager {
public:
    virtual ~ITemplateManager() = default;

    virtual bool reload(const std::string& templates_json_path) = 0;
    virtual std::optional<std::string> get_template(const std::string& tpl_type) const = 0;
    virtual std::optional<std::string> get_template_by_type(const std::string& tpl_type) const = 0;
};

/// 模板管理主实现，负责 JSON 热加载和默认模板回退。
class TemplateManager : public ITemplateManager {
public:
    explicit TemplateManager(std::string templates_json_path = "config/prompt/templates.json");

    bool reload(const std::string& templates_json_path) override;
    std::optional<std::string> get_template(const std::string& tpl_type) const override;
    std::optional<std::string> get_template_by_type(const std::string& tpl_type) const override;

private:
    /// 当前模板 JSON 文件路径。
    std::string templates_json_path_;
    /// 保护模板缓存。
    mutable std::shared_mutex mutex_;
    /// `tpl_type -> 模板内容` 索引。
    std::unordered_map<std::string, std::string> templates_by_type_;
    /// 默认模板类型。
    std::optional<std::string> default_tpl_type_;
};

/// 创建模板管理器实例。
std::unique_ptr<ITemplateManager> create_template_manager(
    const std::string& templates_json_path = "config/prompt/templates.json"
);

} // namespace master_agent::prompt
