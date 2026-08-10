#include "include/skill_content_loader.h"

#include "include/skill_utils.h"

namespace master_agent::skill {

/// 正文读取路径：
/// skill_library_dir / skill_bodies / body_file
/// 这里统一封装后，其他模块就不用关心路径细节了。
std::optional<std::string> SkillContentLoader::load_body(
    const std::string& skill_library_dir,
    const SkillRecord& record
) const {
    const std::string text =
        read_text_file(build_body_file_path(skill_library_dir, record.body_file));
    if (text.empty()) {
        return std::nullopt;
    }
    return text;
}

} // namespace master_agent::skill
