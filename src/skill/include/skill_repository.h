#pragma once

#include "master_agent/skill/skill_engine.h"

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace master_agent::skill {

/// 内存仓库。
/// 这个模块只做“当前内存数据管理”。
class SkillRepository {
public:
    /// 设置当前 Skill 库目录。
    void set_library_dir(std::string skill_library_dir);
    /// 获取当前 Skill 库目录。
    std::string get_library_dir() const;

    /// 用新快照整体替换当前内存数据。
    void replace_all(std::unordered_map<std::string, SkillRecord> skills_by_name);
    /// 获取当前内存数据的一份副本。
    /// 这里的“快照”只是“当前状态拷贝”：
    /// - 不是历史版本
    /// - 不支持回滚
    /// - 主要是为了让上层在锁外做遍历、排序、筛选
    /// 这样既能保护内部容器不被直接改坏，也能减少长时间持锁。
    std::unordered_map<std::string, SkillRecord> snapshot() const;

    /// 按 skill_name 获取单条记录。
    std::optional<SkillRecord> get_record(const std::string& skill_name) const;
    /// 按 name 直接获取单条记录。
    /// 先匹配内部 skill_name，再匹配 name_zh 中的任意一个名称。
    std::optional<SkillRecord> get_record_by_name(const std::string& name) const;

    /// 判断技能是否存在。
    bool contains_skill(const std::string& skill_name) const;
    /// 判断正文文件名是否已被占用。
    bool contains_body_file(const std::string& body_file) const;

    /// 列出全部技能名。
    std::vector<std::string> list_skill_names() const;
private:
    /// 读写锁：读多写少，适合 Skill 快照场景。
    mutable std::shared_mutex mutex_;
    /// 内存中的 skill_name -> SkillRecord 映射。
    std::unordered_map<std::string, SkillRecord> skills_by_name_;
    /// 当前 Skill 库所在目录。
    std::string skill_library_dir_;
};

} // namespace master_agent::skill
