#include "include/skill_utils.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace master_agent::skill {
namespace {

constexpr const char* kSkillsIndexFileName = "skills_index.json";
constexpr const char* kSkillBodiesDirName = "skill_bodies";

} // namespace

/// 去掉首尾空白，主要用于做索引字段归一化。
std::string trim_copy(const std::string& input) {
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
        start++;
    }

    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        end--;
    }

    return input.substr(start, end - start);
}

/// 当前只做 ASCII 小写化，足够处理英文 skill_name/tag。
std::string to_lower_ascii_copy(std::string input) {
    for (char& ch : input) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return input;
}

/// 读全文，失败时返回空字符串。
/// 这里保持接口尽量简单，把错误处理留给上层决定。
std::string read_text_file(const std::filesystem::path& file_path) {
    std::ifstream input(file_path, std::ios::binary);
    if (!input.is_open()) {
        return "";
    }

    std::ostringstream oss;
    oss << input.rdbuf();
    return oss.str();
}

/// 覆盖写文本文件；同时确保父目录存在。
bool write_text_file(const std::filesystem::path& file_path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(file_path.parent_path(), ec);
    if (ec) {
        return false;
    }

    std::ofstream output(file_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }

    output << content;
    return output.good();
}

/// 校验正文文件名是否安全，避免路径穿越和非法扩展名。
bool is_safe_body_file(const std::string& body_file) {
    if (body_file.empty()) {
        return false;
    }

    const std::filesystem::path path(body_file);
    if (path.is_absolute()) {
        return false;
    }

    if (body_file.find("..") != std::string::npos) {
        return false;
    }

    return to_lower_ascii_copy(path.extension().string()) == ".txt";
}

/// 生成索引文件完整路径。
std::filesystem::path build_index_file_path(const std::string& skill_library_dir) {
    return std::filesystem::path(skill_library_dir) / kSkillsIndexFileName;
}

/// 生成正文文件完整路径。
std::filesystem::path build_body_file_path(
    const std::string& skill_library_dir,
    const std::string& body_file
) {
    return std::filesystem::path(skill_library_dir) / kSkillBodiesDirName / body_file;
}

} // namespace master_agent::skill
