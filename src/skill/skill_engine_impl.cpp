#include "include/skill_engine_impl.h"

#include "include/skill_utils.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "../../third_party/rapidjson-1.1.0/include/rapidjson/stringbuffer.h"
#include "../../third_party/rapidjson-1.1.0/include/rapidjson/writer.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <utility>

namespace master_agent::skill {
namespace {

constexpr const char* kDefaultSkillLibraryDir = "config/skill";

using rapidjson::StringBuffer;
using rapidjson::Writer;

std::optional<std::string> load_skill_context(
    const std::string& skill_library_dir,
    const SkillRecord& record,
    const SkillContentLoader& loader
) {
    return loader.load_body(skill_library_dir, record);
}

std::string serialize_skill_summary_to_prompt_json(const SkillRecord& record) {
    StringBuffer buffer;
    Writer<StringBuffer> writer(buffer);

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

    writer.EndObject();
    return buffer.GetString();
}

/// 生成面向 Prompt 的精简 JSON：
/// 只保留帮助模型理解和执行 Skill 的核心字段，避免把维护性字段塞进上下文。
std::string serialize_skill_detail_to_prompt_json(
    const SkillRecord& record,
    const std::string& context
) {
    StringBuffer buffer;
    Writer<StringBuffer> writer(buffer);

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

    writer.Key("context");
    writer.String(context.c_str());

    writer.EndObject();
    return buffer.GetString();
}

/// 校验Skill元数据记录是否合规
/// @param record 待校验的Skill记录对象
/// @return 元数据合法返回true，否则返回false
/// 合法性检查包含：技能名称非空、描述非空、主体文件路径安全、至少存在一个有效标签
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

/// 构造函数只做两件事：
/// 1. 确定 Skill 库目录
/// 2. 调用 ​reloadSkillLibrary(...)​ 把磁盘上的 Skill 库加载进内存
SkillEngineImpl::SkillEngineImpl(
    data_log::IDataLogService* log_service,
    exception::IExceptionManager* exception_mgr,
    std::string skill_library_dir
) : log_service_(log_service),
    exception_mgr_(exception_mgr) {
    (void)log_service_;
    (void)exception_mgr_;

    const std::string dir = trim_copy(skill_library_dir);
    repository_.set_library_dir(dir.empty() ? kDefaultSkillLibraryDir : dir);
    reloadSkillLibrary(repository_.get_library_dir());
}

/// 注册新 Skill：
/// 先把新 Skill 合法化，再把它写入索引和正文文件，最后刷新内存。
bool SkillEngineImpl::registerSkill(const SkillRecord& record) {
    SkillRecord normalized = record;
    normalized.skill_name = trim_copy(normalized.skill_name);
    normalized.category_tag = trim_copy(normalized.category_tag);
    normalized.description = trim_copy(normalized.description);
    normalized.body_file = trim_copy(normalized.body_file);
    if (!is_valid_record_meta(normalized) || trim_copy(normalized.context).empty()) {
        return false;
    }

    const std::string skill_library_dir = repository_.get_library_dir();
    if (repository_.contains_skill(normalized.skill_name)) {
        return false;
    }
    if (repository_.contains_body_file(normalized.body_file)) {
        return false;
    }

    std::unordered_map<std::string, SkillRecord> next_skills = repository_.snapshot();
    next_skills.emplace(normalized.skill_name, normalized);

    if (!store_.save_index(skill_library_dir, next_skills)) {
        return false;
    }
    if (!write_text_file(build_body_file_path(skill_library_dir, normalized.body_file), normalized.context)) {
        return false;
    }

    normalized.context.clear();
    next_skills[normalized.skill_name] = std::move(normalized);
    repository_.replace_all(std::move(next_skills));
    return true;
}

/// 更新 Skill ：
/// 思路和注册类似，只是多了“删除旧的正文文件”的处理。
bool SkillEngineImpl::updateSkill(const std::string& skill_name, const SkillRecord& record) {
    const std::string target_name = trim_copy(skill_name);
    if (target_name.empty()) {
        return false;
    }

    SkillRecord normalized = record;
    normalized.skill_name = trim_copy(normalized.skill_name);
    normalized.category_tag = trim_copy(normalized.category_tag);
    normalized.description = trim_copy(normalized.description);
    normalized.body_file = trim_copy(normalized.body_file);
    if (normalized.skill_name != target_name) {
        return false;
    }
    if (!is_valid_record_meta(normalized) || trim_copy(normalized.context).empty()) {
        return false;
    }

    const auto current_record = repository_.get_record(target_name);
    if (!current_record.has_value()) {
        return false;
    }

    if (current_record->body_file != normalized.body_file &&
        repository_.contains_body_file(normalized.body_file)) {
        return false;
    }

    std::unordered_map<std::string, SkillRecord> next_skills = repository_.snapshot();
    next_skills[target_name] = normalized;

    const std::string skill_library_dir = repository_.get_library_dir();
    if (!store_.save_index(skill_library_dir, next_skills)) {
        return false;
    }
    if (!write_text_file(build_body_file_path(skill_library_dir, normalized.body_file), normalized.context)) {
        return false;
    }

    if (current_record->body_file != normalized.body_file) {
        std::error_code ec;
        std::filesystem::remove(build_body_file_path(skill_library_dir, current_record->body_file), ec);
    }

    normalized.context.clear();
    next_skills[target_name] = std::move(normalized);
    repository_.replace_all(std::move(next_skills));
    return true;
}

/// 删除 Skill ：   
/// 把索引中的记录删掉，再把正文文件删掉，再更新内存。
bool SkillEngineImpl::deleteSkill(const std::string& skill_name) {
    const std::string target_name = trim_copy(skill_name);
    if (target_name.empty()) {
        return false;
    }

    const auto record = repository_.get_record(target_name);
    if (!record.has_value()) {
        return false;
    }

    std::unordered_map<std::string, SkillRecord> next_skills = repository_.snapshot();
    next_skills.erase(target_name);

    const std::string skill_library_dir = repository_.get_library_dir();
    if (!store_.save_index(skill_library_dir, next_skills)) {
        return false;
    }

    std::error_code ec;
    std::filesystem::remove(build_body_file_path(skill_library_dir, record->body_file), ec);
    if (ec) {
        return false;
    }

    repository_.replace_all(std::move(next_skills));
    return true;
}

/// 路由主流程：
/// 1. 只在元数据层做简单命中判断
/// 2. 命中条件仅限 name_zh / tag / description
/// 3. 当前设计要求命中结果直接披露完整正文层，因此返回阶段总是加载详情
///
/// 优化：避免全量拷贝 snapshot，改为持锁遍历 + 收集命中 skill_name，
/// 然后在锁外逐个查询详情。内存占用从 O(N) 降至 O(k)，N=总数，k=命中数。
SkillRouteResult SkillEngineImpl::routeSkills(const SkillRouteRequest& request) const {
    SkillRouteResult result;

    const std::string query = trim_copy(request.query);
    if (query.empty()) {
        return result;
    }

    std::vector<SkillSearchHit> hits;

    // 第一阶段：持锁遍历，只收集命中的 skill_name 和 hit 元数据
    {
        std::shared_lock<std::shared_mutex> lock(repository_.mutex_);
        hits.reserve(repository_.skills_by_name_.size());

        for (const auto& [_, record] : repository_.skills_by_name_) {
            if (!record.enabled) {
                continue;
            }

            SkillSearchHit hit;
            hit.skill_name = record.skill_name;
            hit.description = record.description;
            hit.loaded_level = SkillDisclosureLevel::MetaOnly;

            if (!matcher_.match_meta(query, record, hit)) {
                continue;
            }
            hits.push_back(std::move(hit));
        }
    }
    // 锁已释放

    std::sort(
        hits.begin(),
        hits.end(),
        [](const SkillSearchHit& lhs, const SkillSearchHit& rhs) {
            if (lhs.matched_name != rhs.matched_name) {
                return lhs.matched_name > rhs.matched_name;
            }
            if (lhs.matched_tag != rhs.matched_tag) {
                return lhs.matched_tag > rhs.matched_tag;
            }
            if (lhs.matched_description != rhs.matched_description) {
                return lhs.matched_description > rhs.matched_description;
            }
            return lhs.skill_name < rhs.skill_name;
        }
    );

    result.total_candidates = hits.size();

    if (request.top_k > 0 && hits.size() > request.top_k) {
        hits.resize(request.top_k);
    }

    // 第二阶段：命中后直接回填 Prompt JSON。
    const std::string skill_library_dir = repository_.get_library_dir();
    for (const auto& hit : hits) {
        const auto record = repository_.get_record(hit.skill_name);
        if (!record.has_value()) {
            continue;
        }

        const auto context = load_skill_context(skill_library_dir, record.value(), loader_);
        if (!context.has_value()) {
            continue;
        }

        result.matched_skill_json_list.push_back(
            serialize_skill_detail_to_prompt_json(record.value(), context.value())
        );
    }

    return result;
}

/// 这两个接口直接委托给 repository_。
std::vector<std::string> SkillEngineImpl::getAllSkillNames() const {
    return repository_.list_skill_names();
}

std::vector<std::string> SkillEngineImpl::getAllSkillSummaries() const {
    const std::unordered_map<std::string, SkillRecord> snapshot = repository_.snapshot();
    std::vector<SkillRecord> ordered;
    ordered.reserve(snapshot.size());
    for (const auto& [_, record] : snapshot) {
        ordered.push_back(record);
    }

    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const SkillRecord& lhs, const SkillRecord& rhs) {
            return lhs.skill_name < rhs.skill_name;
        }
    );

    std::vector<std::string> summaries;
    summaries.reserve(ordered.size());
    for (const SkillRecord& record : ordered) {
        summaries.push_back(serialize_skill_summary_to_prompt_json(record));
    }
    return summaries;
}

/// 获取单个 Skill 详情时，同时支持 skill_name 与 name_zh 精确查询。
std::optional<std::string> SkillEngineImpl::getSkillDetail(const std::string& name) const {
    const auto record = repository_.get_record_by_name(name);
    if (!record.has_value()) {
        return std::nullopt;
    }
    const auto context = load_skill_context(repository_.get_library_dir(), record.value(), loader_);
    if (!context.has_value()) {
        return std::nullopt;
    }
    return serialize_skill_detail_to_prompt_json(record.value(), context.value());
}

/// 重载整个 Skill 库：
/// - 先读索引
/// - 再逐个确认正文文件都能读到
/// - 全部成功后再整体替换内存仓库
/// - 清空 loader 缓存，避免脏读
bool SkillEngineImpl::reloadSkillLibrary(const std::string& skill_library_dir) {
    const std::string target_dir = trim_copy(skill_library_dir);
    const std::string dir = target_dir.empty() ? repository_.get_library_dir() : target_dir;
    if (dir.empty()) {
        return false;
    }

    std::unordered_map<std::string, SkillRecord> next_skills;
    if (!store_.load_index(dir, next_skills)) {
        return false;
    }

    for (const auto& [_, record] : next_skills) {
        const auto body_text = loader_.load_body(dir, record);
        if (!body_text.has_value()) {
            return false;
        }
    }

    loader_.clear_cache();
    repository_.set_library_dir(dir);
    repository_.replace_all(std::move(next_skills));
    return true;
}

/// 对外工厂函数。
std::unique_ptr<ISkillEngine> createSkillEngine(
    data_log::IDataLogService* log_service,
    exception::IExceptionManager* exception_mgr
) {
    return std::make_unique<SkillEngineImpl>(
        log_service,
        exception_mgr,
        kDefaultSkillLibraryDir
    );
}

} // namespace master_agent::skill
