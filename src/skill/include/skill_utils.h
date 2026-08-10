#pragma once

#include <filesystem>
#include <string>

namespace master_agent::skill {

/// 去掉字符串首尾空白。
std::string trim_copy(const std::string& input);
/// 只做 ASCII 范围的小写化，够当前索引和匹配初版使用。
std::string to_lower_ascii_copy(std::string input);

/// 读取文本文件全文；失败时返回空字符串。
std::string read_text_file(const std::filesystem::path& file_path);
/// 覆盖写文本文件；失败时返回 false。
bool write_text_file(const std::filesystem::path& file_path, const std::string& content);

/// 校验 body_file 是否安全可用：
/// - 不能为空
/// - 不能是绝对路径
/// - 不能包含 ..
/// - 必须是 .txt
bool is_safe_body_file(const std::string& body_file);

/// 生成 skills_index.json 的完整路径。
std::filesystem::path build_index_file_path(const std::string& skill_library_dir);
/// 生成正文文件的完整路径。
std::filesystem::path build_body_file_path(
    const std::string& skill_library_dir,
    const std::string& body_file
);

} // namespace master_agent::skill
