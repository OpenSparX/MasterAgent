#include "include/prompt_template_analyzer.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace master_agent::prompt {

/// 保存模板文本和分析选项。
PromptTemplateAnalyzer::PromptTemplateAnalyzer(
    std::string tpl,
    PromptTemplateAnalysisOptions options
) : template_(std::move(tpl)), options_(options) {}

/// 返回裁剪后的字符串副本。
std::string PromptTemplateAnalyzer::trim_copy(const std::string& input) {
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
        start++;
    }

    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        end--;
    }

    return input.substr(start, end - start);
}

/// 统一生成带位置的语法错误。
void PromptTemplateAnalyzer::append_syntax_error(
    std::vector<std::string>& syntax_errors,
    const std::string& message,
    size_t position
) {
    std::ostringstream oss;
    oss << message << " (pos=" << position << ")";
    syntax_errors.push_back(oss.str());
}

/// 根据配置规范化变量名。
std::string PromptTemplateAnalyzer::normalize_variable_name(const std::string& input) const {
    if (!options_.trim_variable_name) {
        return input;
    }
    return trim_copy(input);
}

/// 单次扫描模板，收集占位符和语法错误。
TemplateAnalysisReport PromptTemplateAnalyzer::analyze() const {
    TemplateAnalysisReport report;

    size_t pos = 0;
    while (pos < template_.size()) {
        if (pos + 1 < template_.size() && template_[pos] == '{' && template_[pos + 1] == '{') {
            pos += 2;
            continue;
        }

        if (pos + 1 < template_.size() && template_[pos] == '}' && template_[pos + 1] == '}') {
            pos += 2;
            continue;
        }

        if (template_[pos] == '{') {
            const size_t end = template_.find('}', pos + 1);
            if (end == std::string::npos) {
                append_syntax_error(report.syntax_errors, "unclosed '{'", pos);
                break;
            }

            const std::string raw_name = template_.substr(pos + 1, end - pos - 1);
            const std::string name = normalize_variable_name(raw_name);

            if (name.empty()) {
                append_syntax_error(report.syntax_errors, "empty placeholder", pos);
            } else if (std::find(
                           report.placeholders.begin(),
                           report.placeholders.end(),
                           name
                       ) == report.placeholders.end()) {
                report.placeholders.push_back(name);
            }

            pos = end + 1;
            continue;
        }

        if (template_[pos] == '}') {
            append_syntax_error(report.syntax_errors, "orphan '}'", pos);
        }

        pos++;
    }

    return report;
}

/// 基于静态分析结果补充变量匹配信息。
ValidationReport PromptTemplateAnalyzer::validate(const VariableMap& variables) const {
    const TemplateAnalysisReport analysis = analyze();

    ValidationReport report;
    report.placeholders = analysis.placeholders;
    report.syntax_errors = analysis.syntax_errors;

    for (const auto& name : report.placeholders) {
        if (variables.find(name) == variables.end()) {
            report.missing_variables.push_back(name);
        }
    }

    for (const auto& [key, value] : variables) {
        (void)value;
        if (std::find(report.placeholders.begin(), report.placeholders.end(), key) ==
            report.placeholders.end()) {
            report.unused_variables.push_back(key);
        }
    }

    return report;
}

/// 返回模板中去重后的占位符列表。
std::vector<std::string> PromptTemplateAnalyzer::extract_placeholders() const {
    return analyze().placeholders;
}

/// 一次性分析入口。
TemplateAnalysisReport analyze_prompt_template(
    const std::string& tpl,
    PromptTemplateAnalysisOptions options
) {
    return PromptTemplateAnalyzer(tpl, options).analyze();
}

/// 一次性校验入口。
ValidationReport validate_prompt_template(
    const std::string& tpl,
    const VariableMap& variables,
    PromptTemplateAnalysisOptions options
) {
    return PromptTemplateAnalyzer(tpl, options).validate(variables);
}

/// 一次性提取占位符入口。
std::vector<std::string> extract_prompt_placeholders(
    const std::string& tpl,
    PromptTemplateAnalysisOptions options
) {
    return PromptTemplateAnalyzer(tpl, options).extract_placeholders();
}

} // namespace master_agent::prompt
