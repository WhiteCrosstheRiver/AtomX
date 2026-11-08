#pragma once
#include "project_archive.hpp"
#include <cstring>
#include <fstream>

// Named modifier presets kept outside any project. A template stores the parameters of a
// list of pipeline nodes, using the project archive's field-by-field modifier format.
namespace atomx::templates {
struct Template {
    std::string name;
    std::vector<ModifierNode> nodes;
};

inline constexpr char fileMagic[8] = {'A', 'T', 'X', 'T', 'P', 'L', '0', '1'};
inline constexpr size_t templateLimit = 256;
inline constexpr size_t nameLimit = 128;

inline bool validName(const std::string &name) {
    return !name.empty() && name.size() <= nameLimit && name.find_first_not_of(" \t") != std::string::npos;
}

// Writes to a temporary file first, so a failed save leaves the old file intact.
inline void save(const std::filesystem::path &path, const std::vector<Template> &templates) {
    document::valid(templates.size() <= templateLimit);
    auto staged = path;
    staged += L".tmp";
    {
        std::ofstream file(staged, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("Cannot write the modifier template file");
        document::Writer writer{file};
        writer.bytes(fileMagic, sizeof(fileMagic));
        writer.value(uint64_t(templates.size()));
        for (const auto &entry : templates) {
            writer.text(entry.name);
            project::writeGraph(writer, entry.nodes);
        }
        file.flush();
        if (!file) throw std::runtime_error("Cannot write the modifier template file");
    }
    std::error_code error;
    std::filesystem::rename(staged, path, error);
    if (error) {
        std::filesystem::remove(staged, error);
        throw std::runtime_error("Cannot replace the modifier template file");
    }
}

// A missing file is an empty list. A damaged file is an error, so it is never overwritten
// without the user seeing that it could not be read.
inline std::vector<Template> load(const std::filesystem::path &path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return {};
    const auto size = std::filesystem::file_size(path, error);
    if (error) throw std::runtime_error("Cannot read the modifier template file");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read the modifier template file");
    try {
        document::Reader reader{file, uint64_t(size)};
        char magic[sizeof(fileMagic)]{};
        reader.bytes(magic, sizeof(magic));
        document::valid(std::memcmp(magic, fileMagic, sizeof(magic)) == 0);
        std::vector<Template> templates(reader.count(8, templateLimit));
        for (auto &entry : templates) {
            entry.name = reader.text();
            document::valid(validName(entry.name));
            entry.nodes = project::readGraph(reader);
        }
        return templates;
    } catch (const std::exception &) {
        throw std::runtime_error("The modifier template file is damaged or from another AtomX version");
    }
}

inline std::filesystem::path defaultPath() {
    return desktop::settingsPath().parent_path() / "modifier-templates.atomx-templates";
}
} // namespace atomx::templates
