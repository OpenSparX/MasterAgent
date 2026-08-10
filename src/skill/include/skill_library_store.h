#pragma once

#include "master_agent/skill/skill_engine.h"

#include <string>
#include <unordered_map>

namespace master_agent::skill {

/// Skill 库索引文件读写器。
/// 这个模块只关心 skills_index.json：
/// - load_index: 从磁盘读出 Skill 元数据
/// - save_index: 把当前元数据写回磁盘
/// 正文 .txt 文件不在这里处理，正文加载交给 SkillContentLoader。
class SkillLibraryStore {
public:
    /// 从技能库目录加载索引文件。
    bool load_index(
        const std::string& skill_library_dir,
        std::unordered_map<std::string, SkillRecord>& skills_by_name
    ) const;

    /// 把当前索引数据写回技能库目录。
    bool save_index(
        const std::string& skill_library_dir,
        const std::unordered_map<std::string, SkillRecord>& skills_by_name
    ) const;
};

} // namespace master_agent::skill
