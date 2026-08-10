#include "include/skill_repository.h"

#include "include/skill_utils.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace master_agent::skill {

/// Skill 库目录不参与匹配，只是给上层保存“当前在操作哪个库”。
void SkillRepository::set_library_dir(std::string skill_library_dir) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    skill_library_dir_ = std::move(skill_library_dir);
}

std::string SkillRepository::get_library_dir() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return skill_library_dir_;
}

/// 用整份新数据替换旧数据。
/// 这种整体替换比逐条打补丁更容易保证一致性。
void SkillRepository::replace_all(std::unordered_map<std::string, SkillRecord> skills_by_name) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    skills_by_name_ = std::move(skills_by_name);
}

/// 返回副本而不是引用，避免上层拿到内部容器后破坏封装。
/// 这里的 snapshot 本质上就是“当前状态拷贝”，
/// 让上层可以在锁外做排序、筛选、打分这些只读计算。
std::unordered_map<std::string, SkillRecord> SkillRepository::snapshot() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return skills_by_name_;
}

/// 单条查询：给详情读取、更新、删除这些点查场景使用。
std::optional<SkillRecord> SkillRepository::get_record(const std::string& skill_name) const {
    const std::string target = trim_copy(skill_name);
    if (target.empty()) {
        return std::nullopt;
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = skills_by_name_.find(target);
    if (it == skills_by_name_.end()) {
        return std::nullopt;
    }

    return it->second;
}

/// 允许调用方直接按 name 查询：
/// - 如果传的是内部 skill_name，直接命中
/// - 否则再尝试精确匹配 name_zh 中的任意一个中文名称
std::optional<SkillRecord> SkillRepository::get_record_by_name(const std::string& name) const {
    const std::string target = trim_copy(name);
    if (target.empty()) {
        return std::nullopt;
    }

    const std::string normalized_target = to_lower_ascii_copy(target);

    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = skills_by_name_.find(target);
    if (it != skills_by_name_.end()) {
        return it->second;
    }

    for (const auto& [_, record] : skills_by_name_) {
        for (const std::string& candidate_name : record.name_zh) {
            if (to_lower_ascii_copy(trim_copy(candidate_name)) == normalized_target) {
                return record;
            }
        }
    }

    return std::nullopt;
}

/// 判断 skill_name 是否已经存在。
bool SkillRepository::contains_skill(const std::string& skill_name) const {
    const std::string target = trim_copy(skill_name);
    if (target.empty()) {
        return false;
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    return skills_by_name_.find(target) != skills_by_name_.end();
}

/// 判断 body_file 是否被占用，避免两个 Skill 指向同一个正文文件。
bool SkillRepository::contains_body_file(const std::string& body_file) const {
    const std::string target = trim_copy(body_file);
    if (target.empty()) {
        return false;
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& [_, record] : skills_by_name_) {
        if (record.body_file == target) {
            return true;
        }
    }
    return false;
}

/// 返回排好序的 skill_name，便于上层稳定展示和测试比较。
std::vector<std::string> SkillRepository::list_skill_names() const {
    std::vector<std::string> names;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    names.reserve(skills_by_name_.size());
    for (const auto& [skill_name, _] : skills_by_name_) {
        names.push_back(skill_name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace master_agent::skill
