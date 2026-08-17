// atomic_file_writer.cpp — Crash-safe file writes via rename
#include <fstream>
#include <string>
#include <cstdio>
#include <filesystem>

namespace sparx::memory {

class AtomicFileWriter {
public:
    static bool write(const std::string& path, const std::string& data) {
        std::string tmp = path + ".tmp";
        {
            std::ofstream ofs(tmp, std::ios::binary);
            if (!ofs) return false;
            ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!ofs) { std::remove(tmp.c_str()); return false; }
        }
        return std::rename(tmp.c_str(), path.c_str()) == 0;
    }

    static std::string read(const std::string& path) {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs) return {};
        auto sz = ifs.tellg();
        ifs.seekg(0);
        std::string buf(static_cast<size_t>(sz), '\0');
        ifs.read(buf.data(), sz);
        return buf;
    }
};

} // namespace sparx::memory
