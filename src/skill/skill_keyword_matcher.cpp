#include "include/skill_keyword_matcher.h"

#include "include/skill_utils.h"

#include <algorithm>
#include <string>

namespace master_agent::skill {
namespace {

bool contains_text(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return false;
    }
    return to_lower_ascii_copy(haystack).find(to_lower_ascii_copy(needle)) != std::string::npos;
}

void append_unique_keyword(std::vector<std::string>& matched_keywords, const std::string& keyword) {
    if (keyword.empty()) {
        return;
    }
    if (std::find(matched_keywords.begin(), matched_keywords.end(), keyword) == matched_keywords.end()) {
        matched_keywords.push_back(keyword);
    }
}

} // namespace

/// 简单元数据命中：
/// - skill_name 包含 query
/// - 任一 tag 包含 query
/// - description 包含 query
/// 只要任意一项命中，就认为当前 Skill 命中。
bool SkillKeywordMatcher::match_meta(
    const std::string& query,
    const SkillRecord& record,
    SkillSearchHit& hit
) const {
    const std::string normalized_query = trim_copy(query);
    if (normalized_query.empty()) {
        return false;
    }

    for (const std::string& name : record.name_zh) {
        if (contains_text(name, normalized_query) || contains_text(normalized_query, name)) {
            hit.matched_name = true;
            append_unique_keyword(hit.matched_keywords, name);
        }
    }

    for (const std::string& tag : record.tag) {
        if (contains_text(tag, normalized_query) || contains_text(normalized_query, tag)) {
            hit.matched_tag = true;
            append_unique_keyword(hit.matched_keywords, tag);
        }
    }

    hit.matched_description =
        contains_text(record.description, normalized_query) ||
        contains_text(normalized_query, record.description);
    if (hit.matched_description) {
        append_unique_keyword(hit.matched_keywords, record.description);
    }

    return hit.matched_name || hit.matched_tag || hit.matched_description;
}

} // namespace master_agent::skill
