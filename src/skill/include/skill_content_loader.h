#pragma once

#include "master_agent/skill/skill_engine.h"

#include <optional>
#include <string>

namespace master_agent::skill {

/// 正文加载器。
/// 这个模块很单纯：根据 body_file 去 skill_bodies 目录读 .txt 内容。
/// 之所以单独拆出来，是为了让“文件路径解析”和“正文读取”不散落在总控逻辑里。
class SkillContentLoader {
public:
    /// 加载单个 Skill 的正文。
    std::optional<std::string> load_body(
        const std::string& skill_library_dir,
        const SkillRecord& record
    ) const;
};

} // namespace master_agent::skill
