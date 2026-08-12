#pragma once

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace master_agent::skill {

struct SkillRecord;

/// 正文加载器。
/// 这个模块很单纯：根据 body_file 去 skill_bodies 目录读 .txt 内容。
/// 之所以单独拆出来，是为了让”文件路径解析”和”正文读取”不散落在总控逻辑里。
///
/// 优化：增加内存缓存，避免对热点 skill 重复磁盘 I/O。
class SkillContentLoader {
public:
    /// 加载单个 Skill 的正文。
    std::optional<std::string> load_body(
        const std::string& skill_library_dir,
        const SkillRecord& record
    ) const;

    /// 清空缓存（在 reloadSkillLibrary 时调用，确保一致性）。
    void clear_cache();

private:
    /// 缓存：body_file -> 正文内容
    mutable std::unordered_map<std::string, std::string> cache_;
    /// 读写锁保护缓存
    mutable std::shared_mutex cache_mutex_;
};

} // namespace master_agent::skill
