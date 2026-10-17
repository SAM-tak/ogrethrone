#include "editor/Level.h"
#include <algorithm>
#include <limits>

namespace throne {
namespace {
constexpr std::string_view levelFormat = "ogrethrone-level";
constexpr LhatInteger levelVersion = 3;
}

SceneInstance& Level::addScene(const std::string& path) {
    if (nextSceneId == std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("Scene ID limit reached");
    scenes.push_back({nextSceneId++, path, {}});
    return scenes.back();
}

SceneInstance* Level::findScene(std::uint32_t id) {
    for (auto& instance : scenes)
        if (instance.id == id) return &instance;
    return nullptr;
}

void Level::save(const std::filesystem::path& path) const {
    lton::Writer out;
    const auto root = out.table();
    out.set(root, "format", out.string(levelFormat));
    out.set(root, "version", lhat_integer(levelVersion));
    const auto list = out.table();
    for (const auto& instance : scenes) {
        const auto item = out.table();
        out.set(item, "id", lhat_integer(instance.id));
        out.set(item, "scene", out.string(instance.scene));
        if (!instance.overrides.empty()) out.set(item, "overrides", writeOverrides(out, instance.overrides));
        out.push(list, item);
    }
    out.set(root, "scenes", list);
    out.set(root, "runners", writeRunners(out, runners));
    out.save(root, path);
}

Level Level::load(const std::filesystem::path& path) {
    const lton::Document document(path);
    document.expect(levelFormat, levelVersion);
    Level result;
    const auto& list = lton::table(lton::field(document.root(), "scenes"));
    for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
        const auto& item = lton::table(lton::at(list, i));
        const auto id = lton::integer(lton::field(item, "id"));
        if (id <= 0 || id >= std::numeric_limits<std::uint32_t>::max() || result.findScene(static_cast<std::uint32_t>(id)))
            lton::invalid();
        result.scenes.push_back({static_cast<std::uint32_t>(id), lton::relativePath(lton::field(item, "scene")),
                                 readOverrides(lton::field(item, "overrides"))});
        result.nextSceneId = std::max(result.nextSceneId, result.scenes.back().id + 1);
    }
    result.runners = readRunners(lton::field(document.root(), "runners"), result.nextRunnerId);
    return result;
}
}
