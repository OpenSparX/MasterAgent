#pragma once

#include "skill_content_loader.h"
#include "skill_keyword_matcher.h"
#include "skill_library_store.h"
#include "skill_repository.h"

#include "master_agent/skill/skill_engine.h"

#include <memory>
#include <string>

namespace master_agent::skill {

/// Skill 引擎内部总控实现。
/// 这个类自己不直接做底层读写和匹配算法，而是把请求分发给下面几个子模块：
/// - repository_：内存中的 Skill 快照
/// - store_：skills_index.json 的读写
/// - loader_：正文 .txt 文件加载
/// - matcher_：关键词匹配和简单打分
class SkillEngineImpl : public ISkillEngine {
public:
    /// 构造时会尝试加载一次默认 Skill 库。
    SkillEngineImpl(
        data_log::IDataLogService* log_service = nullptr,
        exception::IExceptionManager* exception_mgr = nullptr,
        std::string skill_library_dir = "config/skill"
    );

    bool registerSkill(const SkillRecord& record) override;
    bool updateSkill(const std::string& skill_name, const SkillRecord& record) override;
    bool deleteSkill(const std::string& skill_name) override;

    SkillRouteResult routeSkills(const SkillRouteRequest& request) const override;
    std::vector<std::string> getAllSkillNames() const override;
    std::vector<std::string> getAllSkillSummaries() const override;
    std::optional<std::string> getSkillDetail(const std::string& name) const override;

    bool reloadSkillLibrary(const std::string& skill_library_dir) override;

private:
    /// 运行时内存仓库。
    SkillRepository repository_;
    /// 元数据索引文件读写器。
    SkillLibraryStore store_;
    /// 正文加载器。
    SkillContentLoader loader_;
    /// 关键词匹配器。
    SkillKeywordMatcher matcher_;
    /// 预留日志依赖。
    data_log::IDataLogService* log_service_;
    /// 预留异常管理依赖。
    exception::IExceptionManager* exception_mgr_;
};

} // namespace master_agent::skill
