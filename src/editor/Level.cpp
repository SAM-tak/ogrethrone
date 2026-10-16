#include "editor/Level.h"

namespace throne {
namespace {
constexpr std::string_view levelFormat = "ogrethrone-level";
constexpr LhatInteger levelVersion = 2;
}

void Level::save(const std::filesystem::path& path) const {
    lton::Writer out;
    const auto root = out.table();
    out.set(root, "format", out.string(levelFormat));
    out.set(root, "version", lhat_integer(levelVersion));
    const auto list = out.table();
    for (const auto& scene : scenes) out.push(list, out.string(scene));
    out.set(root, "scenes", list);
    out.set(root, "runners", writeRunners(out, runners));
    out.save(root, path);
}

Level Level::load(const std::filesystem::path& path) {
    const lton::Document document(path);
    document.expect(levelFormat, levelVersion);
    Level result;
    const auto& list = lton::table(lton::field(document.root(), "scenes"));
    for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i)
        result.scenes.push_back(lton::relativePath(lton::at(list, i)));
    result.runners = readRunners(lton::field(document.root(), "runners"), result.nextRunnerId);
    return result;
}
}
