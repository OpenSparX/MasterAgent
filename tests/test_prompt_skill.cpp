/**
 * @file test_prompt_skill.cpp
 * @brief Verifies the module-owner Prompt templates and Skill library.
 */

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#include "master_agent/prompt/prompt_engine.h"
#include "master_agent/skill/skill_engine.h"

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    auto prompt = master_agent::prompt::createPromptEngine();
    expect(prompt != nullptr, "Prompt engine factory must succeed");
    expect(
        prompt->reloadTemplates("config/prompt/templates.json"),
        "Prompt templates must load from the supplied configuration");

    master_agent::prompt::PromptContext prompt_context;
    prompt_context.tpl_type = "nav";
    prompt_context.context = "navigate home";
    prompt_context.memory_context = "last_route";
    prompt_context.vehicle_state = "driving";
    const std::string rendered = prompt->buildPrompt(prompt_context);
    expect(
        rendered ==
            u8"你是导航助手，你的任务是根据上下文和用户指令生成符合要求的导航。"
            u8"上下文：memory=last_route; state=driving; user=navigate home",
        "Prompt rendering must match the supplied nav template");
    expect(
        prompt->selectTemplate("not_exist") ==
            prompt->selectTemplate("default"),
        "Unknown template types must fall back to default");

    auto skill = master_agent::skill::createSkillEngine();
    expect(skill != nullptr, "Skill engine factory must succeed");
    expect(
        skill->reloadSkillLibrary("config/skill"),
        "Skill library must load from the supplied configuration");

    const auto names = skill->getAllSkillNames();
    expect(
        std::find(names.begin(), names.end(), "music.play") != names.end(),
        "Skill library must contain music.play");
    expect(
        std::find(
            names.begin(), names.end(),
            "scenario.cabin_comfort_adjustment") != names.end(),
        "Skill library must contain cabin comfort scenario");

    master_agent::skill::SkillRouteRequest route;
    route.query = u8"帮我放歌";
    route.top_k = 1;
    const auto routed = skill->routeSkills(route);
    expect(routed.total_candidates >= 1, "Music query must match a Skill");
    expect(
        routed.matched_skill_json_list.size() == 1,
        "top_k must limit the routed Skill list");
    expect(
        routed.matched_skill_json_list.front().find("music.play") !=
            std::string::npos,
        "Music query must route to music.play");

    const auto detail = skill->getSkillDetail(u8"播放音乐");
    expect(detail.has_value(), "Chinese Skill name must resolve");
    expect(
        detail->find(u8"处理播放") != std::string::npos,
        "Skill detail must contain the supplied body text");

    std::cout << "Prompt and Skill module tests passed\n";
    return 0;
}
