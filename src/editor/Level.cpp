#include "editor/Level.h"
#include "editor/Lton.h"
#include <algorithm>
#include <limits>
#include <set>

namespace throne {
namespace {
constexpr std::string_view levelFormat = "ogrethrone-level";
constexpr LhatInteger levelVersion = 1;
}

LevelObject& Level::addCube() {
    if (nextId == std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("Level object ID limit reached");
    LevelObject object;
    object.id = nextId++;
    object.name = "Cube " + std::to_string(object.id);
    objects.push_back(object);
    return objects.back();
}

LevelObject* Level::find(std::uint32_t id) {
    auto it = std::find_if(objects.begin(), objects.end(), [id](const auto& object) { return object.id == id; });
    return it == objects.end() ? nullptr : &*it;
}

Level Level::example() {
    Level result;
    result.addCube();
    auto& second = result.addCube();
    second.position = {2, 0.75f, -1};
    second.scale = {1, 1.5f, 1};
    second.colour = {0.86f, 0.53f, 0.25f};
    return result;
}

void Level::save(const std::filesystem::path& path) const {
    lton::Writer write;
    const auto root = write.table();
    write.set(root, "format", write.string(levelFormat));
    write.set(root, "version", lhat_integer(levelVersion));
    const auto list = write.table();
    for (const auto& object : objects) {
        const auto entry = write.table();
        write.set(entry, "id", lhat_integer(object.id));
        write.set(entry, "name", write.string(object.name));
        write.set(entry, "type", write.string("cube"));
        write.set(entry, "position", write.vector(object.position.data()));
        write.set(entry, "rotation", write.vector(object.rotation.data()));
        write.set(entry, "scale", write.vector(object.scale.data()));
        write.set(entry, "colour", write.vector(object.colour.data()));
        if (!object.script.empty()) write.set(entry, "script", write.string(object.script));
        write.push(list, entry);
    }
    write.set(root, "objects", list);
    write.save(root, path);
}

Level Level::load(const std::filesystem::path& path) {
    const lton::Document document(path);
    document.expect(levelFormat, levelVersion);
    const auto& list = lton::table(lton::field(document.root(), "objects"));
    Level result;
    std::set<std::uint32_t> ids;
    for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
        const auto& entry = lton::table(lton::at(list, i));
        if (lton::string(lton::field(entry, "type")) != "cube") lton::invalid();
        LevelObject object;
        const auto id = lton::integer(lton::field(entry, "id"));
        if (id <= 0 || id >= std::numeric_limits<std::uint32_t>::max()) lton::invalid();
        object.id = static_cast<std::uint32_t>(id);
        if (!ids.insert(object.id).second) lton::invalid();
        object.name = lton::string(lton::field(entry, "name"));
        if (object.name.empty() || object.name.size() > 255 || object.name.find('\0') != std::string::npos)
            lton::invalid();
        lton::vector(lton::field(entry, "position"), object.position.data());
        lton::vector(lton::field(entry, "rotation"), object.rotation.data());
        lton::vector(lton::field(entry, "scale"), object.scale.data());
        lton::vector(lton::field(entry, "colour"), object.colour.data());
        if (const auto script = lton::field(entry, "script"); !lhat_is_nil(script))
            object.script = lton::relativePath(script);
        for (unsigned axis = 0; axis < 3; ++axis)
            if (object.scale.data()[axis] < 0.001f || object.colour[axis] < 0 || object.colour[axis] > 1)
                lton::invalid();
        result.nextId = std::max(result.nextId, object.id + 1);
        result.objects.push_back(std::move(object));
    }
    return result;
}
}
