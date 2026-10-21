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
    const auto settings=out.table();
    const auto& r=rendering;
    for(const auto& v:{std::pair{"grid",r.grid},std::pair{"clustered",r.clustered},std::pair{"debugClusters",r.debugClusters}})
        out.set(settings,v.first,lhat_bool(v.second));
    out.set(settings,"minDistance",lhat_real(r.minDistance));
    out.set(settings,"maxDistance",lhat_real(r.maxDistance));
    out.set(root,"rendering",settings);
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
    out.set(root, "systems", writeSystems(out, systems));
    validateStages(stages);
    const auto schedule = out.table();
    for (const auto& stage : stages) out.push(schedule, out.string(stage));
    out.set(root, "stages", schedule);
    if(!mixer.empty()) out.set(root,"mixer",out.string(mixer));
    out.set(root, "serialExecution", lhat_bool(serialExecution));
    out.save(root, path);
}

Level Level::load(const std::filesystem::path& path) {
    const lton::Document document(path);
    document.expect(levelFormat, levelVersion);
    Level result;
    if(const auto v=lton::field(document.root(),"mixer"); !lhat_is_nil(v)) result.mixer=lton::relativePath(v);
    if(const auto value=lton::field(document.root(),"rendering"); !lhat_is_nil(value)) {
        const auto& t=lton::table(value); auto& r=result.rendering;
        for(const auto& v:{std::pair{"grid",&r.grid},std::pair{"clustered",&r.clustered},std::pair{"debugClusters",&r.debugClusters}})
            if(const auto f=lton::field(t,v.first); !lhat_is_nil(f)) {
                if(!lhat_is_bool(f)) lton::invalid(); *v.second=lhat_as_bool(f);
            }
        for(const auto& v:{std::pair{"minDistance",&r.minDistance},std::pair{"maxDistance",&r.maxDistance}})
            if(const auto f=lton::field(t,v.first); !lhat_is_nil(f)) *v.second=static_cast<float>(lton::number(f));
        if(r.minDistance<=0 || r.maxDistance<=r.minDistance) lton::invalid();
    }
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
    result.systems = readSystems(lton::field(document.root(), "systems"), result.nextSystemId);
    if (const auto schedule = lton::field(document.root(), "stages"); !lhat_is_nil(schedule)) {
        result.stages.clear();
        const auto& list = lton::table(schedule);
        for (std::size_t i = 0, n = lton::sequence(list); i < n; ++i)
            result.stages.emplace_back(lton::string(lton::at(list, i)));
    }
    if (const auto serial = lton::field(document.root(), "serialExecution"); !lhat_is_nil(serial)) {
        if (!lhat_is_bool(serial)) lton::invalid();
        result.serialExecution = lhat_as_bool(serial);
    }
    const bool hasSteam=std::find(result.stages.begin(),result.stages.end(),"SteamAudio")!=result.stages.end();
    const bool hasSoloud=std::find(result.stages.begin(),result.stages.end(),"SoLoud")!=result.stages.end();
    if(!hasSteam && !hasSoloud) {
        const auto at=std::find(result.stages.begin(),result.stages.end(),"Box3DStep");
        if(at!=result.stages.end()) result.stages.insert(at+1,{"SteamAudio","SoLoud"});
    }
    for(const auto& system:result.systems)
        if(system.name=="SteamAudio" || system.name=="SoLoud")
            throw std::runtime_error("User System conflicts with audio engine stage: "+system.name);
    try { validateStages(result.stages); } catch (const std::exception&) { lton::invalid(); }
    return result;
}
}
