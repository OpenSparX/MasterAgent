#include "include/template_manager.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "../../third_party/rapidjson-1.1.0/include/rapidjson/document.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>

namespace master_agent::prompt {
namespace {

using rapidjson::Document;
using rapidjson::Value;

/// 返回裁剪后的字符串副本。
std::string trim_copy(const std::string& input) {
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

/// 将 `{{slot}}` 归一化为 `{slot}`。
std::string normalize_template_syntax(const std::string& input) {
    std::string output;
    output.reserve(input.size());

    size_t pos = 0;
    while (pos < input.size()) {
        if (pos + 1 < input.size() && input[pos] == '{' && input[pos + 1] == '{') {
            output.push_back('{');
            pos += 2;
            continue;
        }

        if (pos + 1 < input.size() && input[pos] == '}' && input[pos + 1] == '}') {
            output.push_back('}');
            pos += 2;
            continue;
        }

        output.push_back(input[pos]);
        pos++;
    }

    return output;
}

/// 读取文件全文。
std::string read_text_file(const std::filesystem::path& file_path) {
    std::ifstream input(file_path, std::ios::binary);
    if (!input.is_open()) {
        return "";
    }

    std::ostringstream oss;
    oss << input.rdbuf();
    return oss.str();
}

/// 读取对象中的字符串字段。
std::string get_json_string(const Value& value, const char* key) {
    if (!value.IsObject() || !value.HasMember(key) || !value[key].IsString()) {
        return "";
    }
    return value[key].GetString();
}

/// 解析单条模板记录。
bool parse_template_record(
    const Value& value,
    std::string& tpl_type,
    std::string& content
) {
    const std::string parsed_tpl_type = trim_copy(get_json_string(value, "tpl_type"));
    const std::string parsed_content = get_json_string(value, "content");
    if (parsed_tpl_type.empty() || parsed_content.empty()) {
        return false;
    }

    tpl_type = parsed_tpl_type;
    content = normalize_template_syntax(parsed_content);
    return true;
}

/// 从单个 JSON 文件解析全部模板。
bool load_templates_from_file(
    const std::string& file_path,
    std::unordered_map<std::string, std::string>& templates_by_type,
    std::optional<std::string>& default_tpl_type
) {
    const std::string text = read_text_file(file_path);
    if (text.empty()) {
        return false;
    }

    Document document;
    if (document.Parse(text.c_str()).HasParseError() || !document.IsObject()) {
        return false;
    }
    if (!document.HasMember("templates") || !document["templates"].IsArray()) {
        return false;
    }

    std::unordered_map<std::string, std::string> next_templates;
    std::optional<std::string> next_default;
    for (const auto& item : document["templates"].GetArray()) {
        std::string tpl_type;
        std::string content;
        if (!parse_template_record(item, tpl_type, content)) {
            return false;
        }

        const auto [_, inserted] = next_templates.emplace(tpl_type, content);
        if (!inserted) {
            return false;
        }

        if (tpl_type == "default") {
            next_default = tpl_type;
        }
    }

    templates_by_type = std::move(next_templates);
    default_tpl_type = std::move(next_default);
    return true;
}

} // namespace

/// 保存当前模板文件路径，首次加载由调用方决定。
TemplateManager::TemplateManager(std::string templates_json_path)
    : templates_json_path_(std::move(templates_json_path)) {}

/// 从单个 JSON 文件刷新模板缓存。
bool TemplateManager::reload(const std::string& templates_json_path) {
    const std::string target_path =
        templates_json_path.empty() ? templates_json_path_ : templates_json_path;
    if (target_path.empty()) {
        return false;
    }

    std::error_code ec;
    const std::filesystem::path path(target_path);
    if (!std::filesystem::exists(path, ec) || ec) {
        return false;
    }
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return false;
    }

    std::unordered_map<std::string, std::string> next_templates;
    std::optional<std::string> next_default_tpl_type;
    if (!load_templates_from_file(target_path, next_templates, next_default_tpl_type)) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    templates_json_path_ = target_path;
    templates_by_type_ = std::move(next_templates);
    default_tpl_type_ = std::move(next_default_tpl_type);
    return true;
}

/// 按类型精确查询模板。
std::optional<std::string> TemplateManager::get_template(const std::string& tpl_type) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = templates_by_type_.find(tpl_type);
    if (it == templates_by_type_.end()) {
        return std::nullopt;
    }

    return it->second;
}

/// 按类型查询；未命中时回退默认模板。
std::optional<std::string> TemplateManager::get_template_by_type(
    const std::string& tpl_type
) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    const auto it = templates_by_type_.find(tpl_type);
    if (it != templates_by_type_.end()) {
        return it->second;
    }

    if (!default_tpl_type_.has_value()) {
        return std::nullopt;
    }

    const auto default_it = templates_by_type_.find(default_tpl_type_.value());
    if (default_it == templates_by_type_.end()) {
        return std::nullopt;
    }

    return default_it->second;
}

/// 创建模板管理器实例。
std::unique_ptr<ITemplateManager> create_template_manager(const std::string& templates_json_path) {
    return std::make_unique<TemplateManager>(templates_json_path);
}

} // namespace master_agent::prompt
