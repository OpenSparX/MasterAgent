#pragma once

#include "prompt_template_types.h"

#include <string>

namespace master_agent::prompt {

/// 缺失变量时的处理策略。
enum class MissingVariablePolicy {
    Throw,
    KeepPlaceholder,
    UseEmpty
};

/// 模板渲染选项。
struct PromptTemplateOptions {
    MissingVariablePolicy missing_policy = MissingVariablePolicy::Throw;
    bool trim_variable_name = true;
};

/// 轻量级模板渲染器，只处理占位符替换。
class PromptRenderer {
public:
    explicit PromptRenderer(std::string tpl, PromptTemplateOptions options = {});
    std::string format(const VariableMap& variables) const;

private:
    /// 裁剪变量名前后的空白。
    static std::string trim_copy(const std::string& input);
    /// 根据配置规范化变量名。
    std::string normalize_variable_name(const std::string& input) const;
    /// 按策略处理缺失变量。
    void handle_missing_variable(const std::string& var_name, std::string& result) const;

    /// 原始模板文本。
    std::string template_;
    /// 当前渲染选项。
    PromptTemplateOptions options_;
};

/// 一次性渲染入口。
std::string format_prompt(
    const std::string& tpl,
    const VariableMap& variables
);

} // namespace master_agent::prompt
