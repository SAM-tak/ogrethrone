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
    scenes.push_back({nextSceneId++, path});
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
        out.set(item, "position", out.vector(instance.transform.position.data()));
        out.set(item, "rotation", out.vector(instance.transform.rotation.data()));
        out.set(item, "scale", out.vector(instance.transform.scale.data()));
        out.push(list, item);
    }
    out.set(root, "scenes", list);
    out.set(root, "runners", writeRunners(out, runners));
    validateStages(stages);
    const auto schedule = out.table();
    for (const auto& stage : stages) {
        const auto item = out.table();
        out.set(item, "name", out.string(stage.name));
        if (!stage.parallel.empty()) {
            const auto members = out.table();
            for (const auto& member : stage.parallel) out.push(members, out.string(member));
            out.set(item, "parallel", members);
        }
        out.push(schedule, item);
    }
    out.set(root, "stages", schedule);
    out.set(root, "serialExecution", lhat_bool(serialExecution));
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
        result.scenes.push_back({static_cast<std::uint32_t>(id), lton::relativePath(lton::field(item, "scene"))});
        // An instance without a transform sits at the level's origin.
        auto& transform = result.scenes.back().transform;
        for (const auto& [key, out] : {std::pair{"position", &transform.position}, std::pair{"rotation", &transform.rotation},
                                       std::pair{"scale", &transform.scale}})
            if (const auto value = lton::field(item, key); !lhat_is_nil(value)) lton::vector(value, out->data());
        for (const float component : {transform.scale.x, transform.scale.y, transform.scale.z})
            if (!(component >= 0.001f)) lton::invalid();
        result.nextSceneId = std::max(result.nextSceneId, result.scenes.back().id + 1);
    }
    result.runners = readRunners(lton::field(document.root(), "runners"), result.nextRunnerId);
    if (const auto schedule = lton::field(document.root(), "stages"); !lhat_is_nil(schedule)) {
        result.stages.clear();
        const auto& list = lton::table(schedule);
        for (std::size_t i = 0, n = lton::sequence(list); i < n; ++i) {
            const auto& item = lton::table(lton::at(list, i));
            ExecutionStage stage{std::string(lton::string(lton::field(item, "name"))), {}};
            if (const auto group = lton::field(item, "parallel"); !lhat_is_nil(group)) {
                const auto& members = lton::table(group);
                for (std::size_t j = 0, count = lton::sequence(members); j < count; ++j)
                    stage.parallel.emplace_back(lton::string(lton::at(members, j)));
            }
            result.stages.push_back(std::move(stage));
        }
    }
    if (const auto serial = lton::field(document.root(), "serialExecution"); !lhat_is_nil(serial)) {
        if (!lhat_is_bool(serial)) lton::invalid();
        result.serialExecution = lhat_as_bool(serial);
    }
    try { validateStages(result.stages); } catch (const std::exception&) { lton::invalid(); }
    return result;
}
}
