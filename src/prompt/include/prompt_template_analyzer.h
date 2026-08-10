#pragma once

#include "prompt_template_types.h"

#include <string>

namespace master_agent::prompt {

/// 模板分析器，负责提取占位符和静态校验。
class PromptTemplateAnalyzer {
public:
    explicit PromptTemplateAnalyzer(
        std::string tpl,
        PromptTemplateAnalysisOptions options = {}
    );

    TemplateAnalysisReport analyze() const;
    ValidationReport validate(const VariableMap& variables) const;
    std::vector<std::string> extract_placeholders() const;

private:
    /// 裁剪变量名前后的空白。
    static std::string trim_copy(const std::string& input);
    /// 追加带位置信息的语法错误。
    static void append_syntax_error(
        std::vector<std::string>& syntax_errors,
        const std::string& message,
        size_t position
    );
    /// 根据配置规范化变量名。
    std::string normalize_variable_name(const std::string& input) const;

private:
    std::string template_;
    PromptTemplateAnalysisOptions options_;
};

/// 一次性分析模板结构。
TemplateAnalysisReport analyze_prompt_template(
    const std::string& tpl,
    PromptTemplateAnalysisOptions options = {}
);

/// 一次性校验模板与变量表是否匹配。
ValidationReport validate_prompt_template(
    const std::string& tpl,
    const VariableMap& variables,
    PromptTemplateAnalysisOptions options = {}
);

/// 一次性提取模板中的占位符。
std::vector<std::string> extract_prompt_placeholders(
    const std::string& tpl,
    PromptTemplateAnalysisOptions options = {}
);

} // namespace master_agent::prompt
