#include "include/skill_library_store.h"

#include "include/skill_utils.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "../../third_party/rapidjson-1.1.0/include/rapidjson/document.h"
#include "../../third_party/rapidjson-1.1.0/include/rapidjson/prettywriter.h"
#include "../../third_party/rapidjson-1.1.0/include/rapidjson/stringbuffer.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace master_agent::skill {
namespace {

using rapidjson::Document;
using rapidjson::PrettyWriter;
using rapidjson::StringBuffer;
using rapidjson::Value;

/// 从 JSON 对象中读取字符串字段；读不到就返回空字符串。
std::string get_json_string(const Value& value, const char* key) {
    if (!value.IsObject() || !value.HasMember(key) || !value[key].IsString()) {
        return "";
    }
    return value[key].GetString();
}

/// 读取布尔字段，不存在时返回调用方给的默认值。
bool get_json_bool(const Value& value, const char* key, bool default_value) {
    if (!value.IsObject() || !value.HasMember(key) || !value[key].IsBool()) {
        return default_value;
    }
    return value[key].GetBool();
}

/// 读取 uint64 字段，不存在时返回默认值。
std::uint64_t get_json_uint64(const Value& value, const char* key, std::uint64_t default_value) {
    if (!value.IsObject() || !value.HasMember(key) || !value[key].IsUint64()) {
        return default_value;
    }
    return value[key].GetUint64();
}

/// 解析 tag 数组，并顺手做空白裁剪。
std::vector<std::string> parse_tags(const Value& value) {
    std::vector<std::string> tags;
    if (!value.IsArray()) {
        return tags;
    }

    for (const auto& item : value.GetArray()) {
        if (!item.IsString()) {
            continue;
        }

        const std::string tag = trim_copy(item.GetString());
        if (!tag.empty()) {
            tags.push_back(tag);
        }
    }

    return tags;
}

/// 这里只检查索引文件里的元数据字段，不读正文。
std::vector<std::string> parse_name_zh(const Value& value) {
    std::vector<std::string> names;
    if (!value.IsArray()) {
        return names;
    }

    for (const auto& item : value.GetArray()) {
        if (!item.IsString()) {
            continue;
        }

        const std::string name = trim_copy(item.GetString());
        if (!name.empty()) {
            names.push_back(name);
        }
    }

    return names;
}

bool is_valid_record_meta(const SkillRecord& record) {
    if (trim_copy(record.skill_name).empty()) {
        return false;
    }
    bool has_valid_name_zh = false;
    for (const std::string& name : record.name_zh) {
        if (!trim_copy(name).empty()) {
            has_valid_name_zh = true;
            break;
        }
    }
    if (!has_valid_name_zh) {
        return false;
    }
    if (trim_copy(record.category_tag).empty()) {
        return false;
    }
    if (trim_copy(record.description).empty()) {
        return false;
    }
    if (!is_safe_body_file(trim_copy(record.body_file))) {
        return false;
    }

    bool has_valid_tag = false;
    for (const std::string& tag : record.tag) {
        if (!trim_copy(tag).empty()) {
            has_valid_tag = true;
            break;
        }
    }

    return has_valid_tag;
}

} // namespace

/// 从 skills_index.json 读出所有 Skill 元数据。
/// 这一层不加载正文，只负责把索引解析成 SkillRecord。
bool SkillLibraryStore::load_index(
    const std::string& skill_library_dir,
    std::unordered_map<std::string, SkillRecord>& skills_by_name
) const {
    const std::string text = read_text_file(build_index_file_path(skill_library_dir));
    if (text.empty()) {
        return false;
    }

    Document document;
    if (document.Parse(text.c_str()).HasParseError() || !document.IsObject()) {
        return false;
    }
    if (!document.HasMember("skills") || !document["skills"].IsArray()) {
        return false;
    }

    std::unordered_map<std::string, SkillRecord> next_skills;
    std::unordered_set<std::string> body_files;

    // 逐条解析并校验 Skill 元数据。
    for (const auto& item : document["skills"].GetArray()) {
        if (!item.IsObject()) {
            return false;
        }
        if (!item.HasMember("name_zh")) {
            return false;
        }
        if (!item.HasMember("tag")) {
            return false;
        }

        SkillRecord record;
        record.skill_name = trim_copy(get_json_string(item, "skill_name"));
        record.name_zh = parse_name_zh(item["name_zh"]);
        record.category_tag = trim_copy(get_json_string(item, "category_tag"));
        record.tag = parse_tags(item["tag"]);
        record.description = trim_copy(get_json_string(item, "description"));
        record.body_file = trim_copy(get_json_string(item, "body_file"));
        record.enabled = get_json_bool(item, "enabled", true);
        record.version = get_json_uint64(item, "version", 1);
        record.updated_at = trim_copy(get_json_string(item, "updated_at"));

        if (!is_valid_record_meta(record)) {
            return false;
        }

        const auto [_, inserted] = next_skills.emplace(record.skill_name, record);
        if (!inserted) {
            return false;
        }

        if (!body_files.emplace(record.body_file).second) {
            return false;
        }

        // 索引声明的正文文件必须真实存在，避免库加载后才发现 body_file 失配。
        if (!std::filesystem::exists(build_body_file_path(skill_library_dir, record.body_file))) {
            return false;
        }
    }

    skills_by_name = std::move(next_skills);
    return true;
}

/// 把当前内存中的元数据回写到 skills_index.json。
/// 注意这里只写索引，不写正文文件。
bool SkillLibraryStore::save_index(
    const std::string& skill_library_dir,
    const std::unordered_map<std::string, SkillRecord>& skills_by_name
) const {
    std::vector<SkillRecord> ordered;
    ordered.reserve(skills_by_name.size());
    for (const auto& [_, record] : skills_by_name) {
        ordered.push_back(record);
    }

    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const SkillRecord& lhs, const SkillRecord& rhs) {
            return lhs.skill_name < rhs.skill_name;
        }
    );

    StringBuffer buffer;
    PrettyWriter<StringBuffer> writer(buffer);
    writer.SetIndent(' ', 2);

    writer.StartObject();
    writer.Key("version");
    writer.Uint64(1);
    writer.Key("skills");
    writer.StartArray();

    for (const SkillRecord& record : ordered) {
        writer.StartObject();
        writer.Key("skill_name");
        writer.String(record.skill_name.c_str());
        writer.Key("name_zh");
        writer.StartArray();
        for (const std::string& name : record.name_zh) {
            writer.String(name.c_str());
        }
        writer.EndArray();
        writer.Key("category_tag");
        writer.String(record.category_tag.c_str());
        writer.Key("tag");
        writer.StartArray();
        for (const std::string& tag : record.tag) {
            writer.String(tag.c_str());
        }
        writer.EndArray();
        writer.Key("description");
        writer.String(record.description.c_str());
        writer.Key("body_file");
        writer.String(record.body_file.c_str());
        writer.Key("enabled");
        writer.Bool(record.enabled);
        writer.Key("version");
        writer.Uint64(record.version);
        writer.Key("updated_at");
        writer.String(record.updated_at.c_str());
        writer.EndObject();
    }

    writer.EndArray();
    writer.EndObject();

    return write_text_file(build_index_file_path(skill_library_dir), buffer.GetString());
}

} // namespace master_agent::skill
