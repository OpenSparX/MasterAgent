#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace master_agent::prompt {

/// 渲染阶段使用的变量表。
using VariableMap = std::unordered_map<std::string, std::string>;

/// 模板分析选项。
struct PromptTemplateAnalysisOptions {
    bool trim_variable_name = true;
};

/// 模板静态分析结果。
struct TemplateAnalysisReport {
    /// 模板中提取出的占位符。
    std::vector<std::string> placeholders;
    /// 模板语法错误列表。
    std::vector<std::string> syntax_errors;

    /// 没有语法错误时返回 true。
    bool ok() const {
        return syntax_errors.empty();
    }
};

/// 模板校验结果。
struct ValidationReport {
    /// 模板中提取出的占位符。
    std::vector<std::string> placeholders;
    /// 模板里引用但变量表中缺失的字段。
    std::vector<std::string> missing_variables;
    /// 变量表中存在但模板未使用的字段。
    std::vector<std::string> unused_variables;
    /// 模板语法错误列表。
    std::vector<std::string> syntax_errors;

    /// 没有缺失变量且没有语法错误时返回 true。
    bool ok() const {
        return missing_variables.empty() && syntax_errors.empty();
    }
};

} // namespace master_agent::prompt
