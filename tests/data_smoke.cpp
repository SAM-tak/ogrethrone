#include "editor/Expansion.h"
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
    runner.behaviours.push_back({"scripts/spin.lh", {{"target", throne::NodeRef{0, parent, {}}},
                                                     {"degreesPerSecond", 45.0},
                                                     {"label", std::string("x")},
                                                     {"enabled", true}}});
    original.save(path);

    const auto loaded = throne::Scene::load(path);
    if (loaded.nodes.size() != 3 || loaded.runners.size() != 1)
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
        throne::Scene::load(reopened.resolve(level.scenes[0].scene)).nodes.size() != 2 ||
        !fs::exists(reopened.resolve("main.lh")))
        return fail("Created project did not reopen with main.lh, its main level and scene");
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

    // Nested prefabs: a stack of two crates; the scene places the stack and
    // overrides one lid, and so does the level's second instance of the scene.
    // An instance node is its prefab's root, so the stack's crates are its children.
    throne::Prefab crate;
    const auto body = crate.addNode("Body").id;
    const auto lid = crate.addNode("Lid", body).id;
    crate.addRunner("Spinner").behaviours.push_back({"scripts/spin.lh", {{"target", throne::NodeRef{0, lid, {}}}}});
    crate.save(directory / "prefabs/crate.prefab.lton");
    throne::Prefab stack;
    const auto stackRoot = stack.addNode("Stack").id;
    const auto bottom = stack.addNode("Bottom", stackRoot).id, top = stack.addNode("Top", stackRoot).id;
    for (const auto id : {bottom, top}) stack.findNode(id)->prefab = "prefabs/crate.prefab.lton";
    stack.findNode(bottom)->overrides.node({lid}).colour = std::array<float, 3>{1, 0, 0};
    stack.save(directory / "prefabs/stack.prefab.lton");
    throne::Scene room;
    room.addNode("Floor");
    const auto pile = room.addNode("Pile").id;
    auto& placed = *room.findNode(pile);
    placed.prefab = "prefabs/stack.prefab.lton";
    placed.position = {3, 0, 0};
    placed.overrides.node({bottom, lid}).position = throne::Vec3{0, 5, 0};
    placed.overrides.fieldsOf({bottom}, 1, 0).fields["degreesPerSecond"] = 10.0;
    room.save(directory / "scenes/room.scene.lton");

    throne::Library library(directory);
    const auto reloaded = throne::Scene::load(directory / "scenes/room.scene.lton");
    const auto expanded = throne::expand(reloaded, library, "scenes/room.scene.lton");
    const auto pileAt = expanded.find({pile}), lidAt = expanded.find({pile, bottom, lid});
    if (!expanded.errors.empty() || expanded.nodes.size() != 6 || lidAt == throne::Expansion::npos)
        return fail("Nested prefab instances did not expand to six nodes");
    if (expanded.nodes[pileAt].node.name != "Pile" || expanded.nodes[pileAt].node.position.x != 3 ||
        expanded.find({pile, stackRoot}) != pileAt || expanded.find({pile, stackRoot, bottom, body, lid}) != lidAt ||
        expanded.find({pile, lid}) != lidAt)
        return fail("An instance did not become its prefab's root, or a path did not search below");
    if (expanded.nodes[lidAt].node.colour[0] != 1 || expanded.nodes[lidAt].node.position.y != 5 ||
        expanded.nodes[expanded.nodes[lidAt].parent].path != throne::NodePath{pile, bottom})
        return fail("Inner and outer overrides were not both applied to the nested lid");
    if (expanded.runners.size() != 2 || expanded.runners[0].instance != throne::NodePath{pile, bottom} ||
        std::get<double>(expanded.runners[0].runner.behaviours[0].fields.at("degreesPerSecond")) != 10 ||
        expanded.runners[1].runner.behaviours[0].fields.count("degreesPerSecond"))
        return fail("Prefab runners were not copied per instance with their field overrides");

    // Moving a subtree into a prefab of its own (as prefab creation does) keeps
    // the overrides that pointed into it working, without rewriting them.
    throne::Prefab shelf;
    const auto shelfRoot = shelf.addNode("Shelf").id;
    const auto board = shelf.addNode("Board", shelfRoot).id;
    const auto book = shelf.addNode("Book", board).id;
    shelf.save(directory / "prefabs/shelf.prefab.lton");
    throne::Scene study;
    const auto placedShelf = study.addNode("Shelf").id;
    study.findNode(placedShelf)->prefab = "prefabs/shelf.prefab.lton";
    study.findNode(placedShelf)->overrides.node({book}).colour = std::array<float, 3>{1, 1, 0};
    throne::Prefab boardPrefab;
    boardPrefab.nodes = {*shelf.findNode(board), *shelf.findNode(book)};
    boardPrefab.nodes[0].parent = 0;
    boardPrefab.save(directory / "prefabs/board.prefab.lton");
    shelf.removeNode(book);
    shelf.findNode(board)->prefab = "prefabs/board.prefab.lton";
    shelf.save(directory / "prefabs/shelf.prefab.lton");
    throne::Library fresh(directory);
    const auto inStudy = throne::expand(study, fresh);
    const auto bookAt = inStudy.find({placedShelf, book});
    if (!inStudy.errors.empty() || bookAt == throne::Expansion::npos ||
        inStudy.nodes[bookAt].path != throne::NodePath{placedShelf, board, book} || inStudy.nodes[bookAt].node.colour[1] != 1)
        return fail("An override did not follow its node into a new prefab");

    // A variant is its base with overrides, both where it is placed and on its own.
    throne::Prefab green;
    green.base = "prefabs/crate.prefab.lton";
    green.overrides.node({}).colour = std::array<float, 3>{0, 1, 0};
    green.overrides.node({lid}).name = "Green Lid";
    green.save(directory / "prefabs/green.prefab.lton");
    throne::Scene yard;
    yard.addNode("Box").prefab = "prefabs/green.prefab.lton";
    yard.nodes[0].overrides.node({lid}).scale = throne::Vec3{2, 2, 2};
    const auto inYard = throne::expand(yard, library);
    const auto alone = throne::expand(throne::Prefab::load(directory / "prefabs/green.prefab.lton"), library,
                                      "prefabs/green.prefab.lton");
    if (!inYard.errors.empty() || inYard.nodes.size() != 2 || inYard.nodes[0].node.name != "Box" ||
        inYard.nodes[0].node.colour[1] != 1 || inYard.nodes[1].node.name != "Green Lid" ||
        inYard.nodes[1].node.scale.x != 2 || alone.nodes.size() != 2 || alone.find({}) != 0 ||
        alone.nodes[0].node.name != "Body" || alone.find({body, lid}) != 1 || alone.nodes[1].node.name != "Green Lid")
        return fail("A variant did not expand to its base with its overrides");

    throne::Prefab loop, twoRoots, rootInstance;
    const auto loopRoot = loop.addNode("Root").id;
    loop.addNode("Self", loopRoot).prefab = "prefabs/loop.prefab.lton";
    loop.save(directory / "prefabs/loop.prefab.lton");
    twoRoots.addNode("A");
    twoRoots.addNode("B");
    twoRoots.save(directory / "prefabs/two.prefab.lton");
    rootInstance.addNode("Crate").prefab = "prefabs/crate.prefab.lton";
    rootInstance.save(directory / "prefabs/rooted.prefab.lton");
    for (const char* file : {"prefabs/loop.prefab.lton", "prefabs/two.prefab.lton", "prefabs/rooted.prefab.lton"}) {
        throne::Scene user;
        user.addNode("Use").prefab = file;
        if (throne::expand(user, library).errors.empty())
            return fail((std::string("A broken prefab was accepted: ") + file).c_str());
    }

    throne::World world;
    world.scenes["scenes/room.scene.lton"] = reloaded;
    world.level.addScene("scenes/room.scene.lton");
    auto& other = world.level.addScene("scenes/room.scene.lton");
    other.overrides.node({reloaded.nodes[0].id}).name = "Other Floor";
    other.transform.position = {10, 0, 0};
    other.transform.rotation = {0, 90, 0};
    world.level.save(directory / "levels/two.level.lton");
    world.level = throne::Level::load(directory / "levels/two.level.lton");
    auto both = throne::expand(world, library);
    if (both.instances.size() != 2 || both.find(1)->nodes[0].node.name != "Floor" ||
        both.find(2)->nodes[0].node.name != "Other Floor" || both.instances[1].transform.position.x != 10 ||
        both.instances[1].transform.rotation.y != 90 || both.instances[0].transform.position.x != 0)
        return fail("Two instances of one scene did not keep their own overrides and transforms");

    fs::remove_all(directory);
    std::printf("Scene, level, prefab expansion, project and list round-tripped\n");
}
