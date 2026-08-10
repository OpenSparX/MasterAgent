#pragma once

#include "master_agent/skill/skill_engine.h"

#include <string>
#include <vector>

namespace master_agent::skill {

/// 关键词匹配器。
/// 这个模块只做“元数据命中判断”，不做文件加载和结果组织。
/// 当前只看三类字段：
/// - skill_name
/// - tag
/// - description
class SkillKeywordMatcher {
public:
    /// 判断 query 是否命中 Skill 元数据，并把命中字段写到 hit 中。
    bool match_meta(
        const std::string& query,
        const SkillRecord& record,
        SkillSearchHit& hit
    ) const;
};

} // namespace master_agent::skill
