#include "include/prompt_engine_impl.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

namespace master_agent::prompt {
namespace {

constexpr const char* kDefaultTemplatesJsonPath = "config/prompt/templates.json";
constexpr const char* kPromptEngineConfigPath = "config/prompt/prompt_engine_config.json";

/// 将对外上下文转成渲染变量表。
VariableMap build_variables(const PromptContext& context) {
    return {
        {"tpl_type", context.tpl_type},
        {"context", context.context},
        {"memory_context", context.memory_context},
        {"emotion_tag", context.emotion_tag},
        {"skill_candidates", context.skill_candidates},
        {"vehicle_state", context.vehicle_state},
    };
}

} // namespace

/// 保存依赖，并在构造时尝试加载一次模板文件。
PromptEngineImpl::PromptEngineImpl(
    std::unique_ptr<ITemplateManager> template_manager,
    data_log::IDataLogService* log_service,
    exception::IExceptionManager* exception_mgr,
    std::string templates_json_path
) : template_manager_(std::move(template_manager)),
    log_service_(log_service),
    exception_mgr_(exception_mgr),
    templates_json_path_(
        templates_json_path.empty() ? kDefaultTemplatesJsonPath : std::move(templates_json_path)
    ) {
    (void)log_service_;
    (void)exception_mgr_;

    if (!template_manager_) {
        template_manager_ = create_template_manager(templates_json_path_);
    }
    if (template_manager_) {
        template_manager_->reload(templates_json_path_);
    }
}

/// 选择模板并完成变量替换。
std::string PromptEngineImpl::buildPrompt(const PromptContext& context) {
    const std::string tpl = selectTemplate(context.tpl_type);
    if (tpl.empty()) {
        return "";
    }

    PromptRenderer renderer(
        tpl,
        PromptTemplateOptions{MissingVariablePolicy::KeepPlaceholder, true}
    );
    return renderer.format(build_variables(context));
}

/// 查询模板；未命中时由 manager 内部回退默认模板。
std::string PromptEngineImpl::selectTemplate(const std::string& tpl_type) const {
    if (!template_manager_) {
        return "";
    }

    const auto tpl = template_manager_->get_template_by_type(tpl_type);
    return tpl.has_value() ? tpl.value() : "";
}

/// 重新加载模板文件；空路径时沿用当前路径。
bool PromptEngineImpl::reloadTemplates(const std::string& templates_path) {
    if (!template_manager_) {
        return false;
    }

    size_t start = 0;
    while (start < templates_path.size() &&
           std::isspace(static_cast<unsigned char>(templates_path[start]))) {
        start++;
    }

    size_t end = templates_path.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(templates_path[end - 1]))) {
        end--;
    }

    const std::string trimmed_path = templates_path.substr(start, end - start);
    const std::string target_path = trimmed_path.empty() ? templates_json_path_ : trimmed_path;
    if (!template_manager_->reload(target_path)) {
        return false;
    }

    templates_json_path_ = target_path;
    return true;
}

/// 创建 Prompt 引擎，并优先读取配置中的模板路径。
std::unique_ptr<IPromptEngine> createPromptEngine(
    data_log::IDataLogService* log_service,
    exception::IExceptionManager* exception_mgr
) {
    std::string templates_json_path = kDefaultTemplatesJsonPath;

    std::ifstream input(kPromptEngineConfigPath, std::ios::binary);
    if (input.is_open()) {
        std::ostringstream oss;
        oss << input.rdbuf();
        const std::string text = oss.str();
        const std::string key = "\"templates_json_path\"";
        const size_t key_pos = text.find(key);
        if (key_pos != std::string::npos) {
            const size_t colon_pos = text.find(':', key_pos + key.size());
            const size_t first_quote =
                colon_pos == std::string::npos ? std::string::npos : text.find('"', colon_pos + 1);
            const size_t second_quote =
                first_quote == std::string::npos ? std::string::npos : text.find('"', first_quote + 1);

            if (second_quote != std::string::npos && second_quote > first_quote + 1) {
                std::string configured =
                    text.substr(first_quote + 1, second_quote - first_quote - 1);

                size_t start = 0;
                while (start < configured.size() &&
                       std::isspace(static_cast<unsigned char>(configured[start]))) {
                    start++;
                }

                size_t end = configured.size();
                while (end > start &&
                       std::isspace(static_cast<unsigned char>(configured[end - 1]))) {
                    end--;
                }

                configured = configured.substr(start, end - start);
                if (!configured.empty()) {
                    templates_json_path = std::move(configured);
                }
            }
        }
    }

    return std::make_unique<PromptEngineImpl>(
        create_template_manager(templates_json_path),
        log_service,
        exception_mgr,
        templates_json_path
    );
}

} // namespace master_agent::prompt
