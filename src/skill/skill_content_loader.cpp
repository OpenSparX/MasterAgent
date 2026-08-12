#include "include/skill_content_loader.h"

#include "include/skill_utils.h"
#include "master_agent/skill/skill_engine.h"

namespace master_agent::skill {

/// 正文读取路径：
/// skill_library_dir / skill_bodies / body_file
/// 这里统一封装后，其他模块就不用关心路径细节了。
///
/// 优化：增加 LRU 风格的读缓存，热点 skill 零磁盘 I/O。
std::optional<std::string> SkillContentLoader::load_body(
    const std::string& skill_library_dir,
    const SkillRecord& record
) const {
    // 先查缓存（共享锁）
    {
        std::shared_lock<std::shared_mutex> lock(cache_mutex_);
        auto it = cache_.find(record.body_file);
        if (it != cache_.end()) {
            return it->second;
        }
    }

    // 缓存未命中，从磁盘读取
    const std::string text =
        read_text_file(build_body_file_path(skill_library_dir, record.body_file));
    if (text.empty()) {
        return std::nullopt;
    }

    // 写入缓存（独占锁）
    {
        std::unique_lock<std::shared_mutex> lock(cache_mutex_);
        cache_[record.body_file] = text;
    }

    return text;
}

/// 清空缓存，在 reloadSkillLibrary 时调用，避免脏读。
void SkillContentLoader::clear_cache() {
    std::unique_lock<std::shared_mutex> lock(cache_mutex_);
    cache_.clear();
}

} // namespace master_agent::skill
