#pragma once

/**
 * @file skill_engine.h
 * @brief Skill 引擎对外接口
 *
 * 当前版本面向“纯文本型 Skill 仓库”，主路径只处理：
 * - skills_index.json 中的元数据索引
 * - skill_bodies/*.txt 中的正文内容
 *
 * 这层只暴露稳定的查询与管理接口，让调用方不感知内部文件加载和匹配细节。
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace data_log {
class IDataLogService;
}

namespace exception {
class IExceptionManager;
}

namespace master_agent::skill {

/// 路由模式。
/// 当前只保留一个最小实现：简单关键词命中。
enum class RouteMode {
    Keyword,
};

/// 渐进式披露层级。
/// 纯文本型 Skill 当前主路径只用到 MetaOnly 和 WithBody。
enum class SkillDisclosureLevel {
    MetaOnly,
    WithBody,
    WithResources,
};

/// Skill 元数据 + 运行时正文容器。
/// 这里的 context 主要给运行时内存对象使用；
/// 在 skills_index.json 中并不会直接落正文，只会落 body_file。
struct SkillRecord {
    /// 技能唯一名，建议用点分风格，例如 music.play。
    std::string skill_name;
    /// 中文名称集合（第一个建议为标准中文名，其余可作为中文别名）。
    /// 该字段用于中文自然语言命中。
    std::vector<std::string> name_zh;
    /// 场景类别标签，建议直接使用复杂场景库中的中文类别。
    std::string category_tag;
    /// 标签列表，用于辅助匹配和归类。
    std::vector<std::string> tag;
    /// 一句话摘要，给简单命中检索和候选展示使用。
    std::string description;
    /// 正文文件名，对应 config/skill/skill_bodies/*.txt。
    std::string body_file;
    /// 运行时加载后的正文内容。
    std::string context;
    /// 是否启用当前 Skill。
    bool enabled = true;
    /// 单个 Skill 的版本号。
    std::uint64_t version = 1;
    /// 更新时间字符串。
    std::string updated_at;
};

/// 路由请求。
/// 当前版本只做简单元数据命中：
/// - skill_name 命中
/// - tag 命中
/// - description 命中
struct SkillRouteRequest {
    /// 用户原始查询。
    std::string query;
    /// 最多返回多少个候选。
    std::size_t top_k = 1;
    /// 期望返回到哪一层披露深度。
    /// 当前实现默认直接披露到正文层。
    SkillDisclosureLevel disclosure_level = SkillDisclosureLevel::WithBody;
};

/// 单个命中结果。
/// 当前不返回复杂分数，只返回命中了哪些元数据字段。
struct SkillSearchHit {
    std::string skill_name;
    std::string description;
    bool matched_name = false;
    bool matched_tag = false;
    bool matched_description = false;
    std::vector<std::string> matched_keywords;
    SkillDisclosureLevel loaded_level = SkillDisclosureLevel::MetaOnly;
};

/// 路由总返回。
/// 对外只暴露可直接拼接进 Prompt 的精简 JSON 字符串列表，
/// 避免调用方再做二次结构转换。
struct SkillRouteResult {
    /// 已按匹配优先级和 top_k 裁剪后的 Prompt JSON 列表。
    std::vector<std::string> matched_skill_json_list;
    std::size_t total_candidates = 0;
};

/// Skill 引擎稳定抽象接口。
/// 调用方应该依赖它，而不是直接依赖某个内部实现类。
class ISkillEngine {
public:
    virtual ~ISkillEngine() = default;

    /// 注册新 Skill。
    virtual bool registerSkill(const SkillRecord& record) = 0;
    /// 按 skill_name 全量更新一个 Skill。
    virtual bool updateSkill(const std::string& skill_name, const SkillRecord& record) = 0;
    /// 删除一个 Skill。
    virtual bool deleteSkill(const std::string& skill_name) = 0;

    /// 按请求路由候选 Skill。
    virtual SkillRouteResult routeSkills(const SkillRouteRequest& request) const = 0;
    /// 获取全量 skill_name 列表。
    virtual std::vector<std::string> getAllSkillNames() const = 0;
    /// 获取全量 Prompt 摘要 JSON 列表。
    virtual std::vector<std::string> getAllSkillSummaries() const = 0;
    /// 获取单个 Skill 的 Prompt JSON。
    /// 查询顺序：
    /// 1. 先精确匹配内部 skill_name
    /// 2. 再精确匹配 name_zh 中的任意一个名称
    virtual std::optional<std::string> getSkillDetail(const std::string& name) const = 0;

    /// 从磁盘重新加载整个 Skill 库。
    virtual bool reloadSkillLibrary(const std::string& skill_library_dir) = 0;
};

/// 创建默认 Skill 引擎实例。
/// 目前会加载 config/skill 下的纯文本型 Skill 仓库。
std::unique_ptr<ISkillEngine> createSkillEngine(
    data_log::IDataLogService* log_service = nullptr,
    exception::IExceptionManager* exception_mgr = nullptr
);

} // namespace master_agent::skill
