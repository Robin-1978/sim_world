#include "world/persistence.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace tide_marks {

void JsonFilePersistence::replaceFile(const std::string& path, const std::string& bytes) {
    const std::string temporaryPath = path + ".tmp";
    std::ofstream output(temporaryPath, std::ios::trunc | std::ios::binary);
    if (!output) throw std::runtime_error("cannot write persistence file: " + path);
    output << bytes;
    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        throw std::runtime_error("cannot flush persistence file: " + path);
    }
    output.close();
    std::error_code error;
    std::filesystem::rename(temporaryPath, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        throw std::runtime_error("cannot replace persistence file: " + path + ": " + error.message());
    }
}

void JsonFilePersistence::saveSnapshot(const std::string& path, const Json& snapshot) const {
    replaceFile(path, snapshot.dump(2) + "\n");
}

void JsonFilePersistence::saveReplay(const std::string& path, const std::vector<Json>& lines) const {
    std::string bytes;
    for (const auto& line : lines) bytes += line.dump() + "\n";
    replaceFile(path, bytes);
}

}  // namespace tide_marks
