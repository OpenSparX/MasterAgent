#include "include/prompt_renderer.h"

#include <cctype>
#include <stdexcept>
#include <utility>

namespace master_agent::prompt {

/// 保存模板文本和渲染选项。
PromptRenderer::PromptRenderer(std::string tpl, PromptTemplateOptions options)
    : template_(std::move(tpl)), options_(options) {}

/// 返回裁剪后的字符串副本。
std::string PromptRenderer::trim_copy(const std::string& input) {
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

/// 根据配置决定是否裁剪变量名。
std::string PromptRenderer::normalize_variable_name(const std::string& input) const {
    if (!options_.trim_variable_name) {
        return input;
    }
    return trim_copy(input);
}

/// 在变量缺失时按策略写入结果。
void PromptRenderer::handle_missing_variable(
    const std::string& var_name,
    std::string& result
) const {
    switch (options_.missing_policy) {
    case MissingVariablePolicy::Throw:
        throw std::runtime_error("missing variable: " + var_name);
    case MissingVariablePolicy::KeepPlaceholder:
        result += "{" + var_name + "}";
        return;
    case MissingVariablePolicy::UseEmpty:
        return;
    }
}

/// 线性扫描模板，处理 `{name}`、`{{` 和 `}}`。
std::string PromptRenderer::format(const VariableMap& variables) const {
    std::string result;
    result.reserve(template_.size());

    size_t pos = 0;
    while (pos < template_.size()) {
        if (pos + 1 < template_.size() && template_[pos] == '{' && template_[pos + 1] == '{') {
            result.push_back('{');
            pos += 2;
            continue;
        }

        if (pos + 1 < template_.size() && template_[pos] == '}' && template_[pos + 1] == '}') {
            result.push_back('}');
            pos += 2;
            continue;
        }

        if (template_[pos] == '{') {
            const size_t end = template_.find('}', pos + 1);
            if (end == std::string::npos) {
                result.append(template_.substr(pos));
                break;
            }

            const std::string raw_name = template_.substr(pos + 1, end - pos - 1);
            const std::string name = normalize_variable_name(raw_name);

            const auto it = variables.find(name);
            if (it != variables.end()) {
                result.append(it->second);
            } else {
                handle_missing_variable(name, result);
            }

            pos = end + 1;
            continue;
        }

        result.push_back(template_[pos]);
        pos++;
    }

    return result;
}

/// 一次性渲染入口。
std::string format_prompt(
    const std::string& tpl,
    const VariableMap& variables
) {
    PromptRenderer t(tpl);
    return t.format(variables);
}

} // namespace master_agent::prompt
