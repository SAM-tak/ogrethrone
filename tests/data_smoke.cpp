#include "editor/Level.h"
#include "editor/Project.h"
#include "editor/Scene.h"
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
/// Reports a failed check and returns the process exit code.
int fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return 1;
}

/// Whether loading @p text as a scene is refused.
bool rejects(const std::filesystem::path& path, const char* text) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    try {
        throne::Scene::load(path);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}
}

int main() {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() / "throne_data_smoke";
    fs::remove_all(directory);
    const auto path = directory / "roundtrip.scene.lton";

    // Hierarchy: reparenting keeps parents before children and refuses cycles.
    auto original = throne::Scene::example();
    const auto parent = original.nodes[0].id, child = original.nodes[1].id;
    auto& grandchild = original.addNode("Grandchild \xE2\x9C\x93", child);
    grandchild.rotation = {0, 45, 10};
    const auto grandchildId = grandchild.id;
    if (!original.reparent(child, parent) || original.reparent(parent, grandchildId) ||
        original.nodes[0].id != parent || original.nodes[2].id != grandchildId || !original.isWithin(grandchildId, parent))
        return fail("Reparenting broke the order or accepted a cycle");
    auto& runner = original.addRunner("Spinner");
    runner.behaviours.push_back({"scripts/spin.lh", {{"target", throne::NodeRef{"", parent}},
                                                     {"degreesPerSecond", 45.0},
                                                     {"label", std::string("x")},
                                                     {"enabled", true}}});
    original.save(path);

    const auto loaded = throne::Scene::load(path);
    if (loaded.nodes.size() != 3 || loaded.nextNodeId != original.nextNodeId || loaded.runners.size() != 1)
        return fail("Node or runner count changed");
    for (std::size_t i = 0; i < loaded.nodes.size(); ++i) {
        const auto& a = original.nodes[i];
        const auto& b = loaded.nodes[i];
        if (a.id != b.id || a.name != b.name || a.parent != b.parent || a.position != b.position ||
            a.rotation != b.rotation || a.scale != b.scale || a.colour != b.colour)
            return fail("Node changed after save/load");
    }
    if (loaded.runners[0].name != "Spinner" || loaded.runners[0].behaviours.size() != 1 ||
        loaded.runners[0].behaviours[0].script != "scripts/spin.lh" ||
        loaded.runners[0].behaviours[0].fields != original.runners[0].behaviours[0].fields)
        return fail("Runner changed after save/load");
    original.removeNode(child);
    if (original.nodes.size() != 1) return fail("Removing a node left its descendants");

    if (!rejects(path, R"(format = "ogrethrone-scene", version = 1, runners = {}, nodes = {
            { id = 1, name = "A", parent = 2, position = {0, 0, 0}, rotation = {0, 0, 0}, scale = {1, 1, 1},
              colour = {1, 1, 1} } })"))
        return fail("A node whose parent comes later was accepted");
    if (!rejects(path, R"(format = "ogrethrone-prefab", version = 1, runners = {}, nodes = {})"))
        return fail("A prefab was accepted as a scene");
    if (!rejects(path, R"(format = "ogrethrone-scene", version = 1, nodes = {}, x = std.io.write("x"))"))
        return fail("A scene that calls a procedure was accepted");

    // A new project reopens with its main level, which loads the example scene.
    const auto project = throne::Project::create(directory, "My Game");
    const auto reopened = throne::Project::open(project.root / throne::Project::markerName);
    const auto level = throne::Level::load(reopened.resolve(reopened.mainLevel));
    if (reopened.name != "My Game" || level.scenes.size() != 1 ||
        throne::Scene::load(reopened.resolve(level.scenes[0])).nodes.size() != 2)
        return fail("Created project did not reopen with its main level and scene");
    try {
        throne::Project::create(directory, "My Game");
        return fail("A project was created in a non-empty folder");
    } catch (const std::runtime_error&) {
    }
    throne::ProjectList list;
    list.touch(directory / "Other");
    list.touch(project.root);
    list.touch(directory / "Other");
    list.save(directory / "projects.lton");
    const auto known = throne::ProjectList::load(directory / "projects.lton");
    if (known.entries.size() != 2 || known.entries[0].root != directory / "Other" ||
        known.entries[1].root != project.root)
        return fail("Project list did not keep most-recent-first order");

    fs::remove_all(directory);
    std::printf("Scene, level, project and list round-tripped\n");
}
