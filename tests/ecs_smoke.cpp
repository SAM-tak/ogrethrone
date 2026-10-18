#include "script/ScriptRuntime.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
/// Fails with a useful diagnostic even in Release builds.
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
/// Writes a script fixture.
void write(const std::filesystem::path& root, const char* name, const char* source) {
    std::ofstream(root / name, std::ios::binary) << source;
}
}

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "throne_ecs_smoke";
    fs::create_directories(root);
    try {
        write(root, "node.lh", R"(module^ test.node
import^ throne
public^let^ Behaviour = def^{
    self^{ @export speed = 10, starts = 0 },
    start = p^self^, node:throne.SceneNode {
        self^.starts := self^.starts + 1
        throne.log("start " .. node.name())
    },
    update = p^self^, node:throne.SceneNode, dt:number^ {
        throne.log(node.name())
        let^ t = node.transform()
        if^ t != nil^ { t.setPosition(t.position() + throne.vec3(self^.speed * dt, 0, 0)) }
    },
}
)");
        write(root, "component.lh", R"(module^ test.component
public^let^ Component = def^{ self^{ speed = 3, ticks = 0 } }
)");
        write(root, "system.lh", R"(module^ test.system
import^ throne
let^ spin = require^ "component.lh"
public^let^ System = def^{
    update = p^self^, runner:throne.Runner, dt:number^ {
        throne.log("system " .. runner.name())
        for^ _^, node in^ throne.query("Transform", "Spin") {
            let^ c = node.component("Spin")
            if^ c fits^ spin.Component {
                c.ticks := c.ticks + 1
                let^ t = node.transform()
                if^ t != nil^ { t.setRotation(t.rotation() + throne.vec3(0, c.speed * dt, 0)) }
            }
        }
    },
}
)");
        throne::World world;
        auto& scene = world.scenes["scene.lton"];
        const auto parent = scene.addNode("Parent").id;
        scene.addNode("Child", parent).behaviours.push_back({"node.lh", {{"speed", 20.0}}});
        scene.nodes[0].behaviours.push_back({"node.lh", {}});
        scene.nodes[0].scriptComponents["Spin"] = {"component.lh", {{"speed", 30.0}}};
        world.level.addScene("scene.lton");
        world.level.addScene("scene.lton");
        world.level.addRunner("Update").behaviours.push_back({"system.lh", {}, "Update", true});
        world.level.stages.insert(world.level.stages.begin() + 3, {"Late", {}});
        scene.save(root / "scene.lton");
        world.level.save(root / "level.lton");
        world.scenes["scene.lton"] = throne::Scene::load(root / "scene.lton");
        world.level = throne::Level::load(root / "level.lton");
        require(world.scenes["scene.lton"].nodes[0].scriptComponents.count("Spin") == 1, "Component lost on save");
        require(world.level.stages[3].name == "Late", "Schedule lost on save");
        throne::Library library(root);
        auto expanded = throne::expand(world, library);
        std::vector<std::string> log;
        throne::ScriptRuntime runtime(world, expanded, root, [&](const std::string& text) { log.push_back(text); },
                                     throne::PlayScope::Level);
        runtime.update(0.5f);
        require(log == std::vector<std::string>{"system Update", "start Parent", "Parent", "start Child", "Child",
                                               "start Parent", "Parent", "start Child", "Child"}, "Stage or hierarchy order wrong");
        require(std::fabs(expanded.instances[0].expansion.nodes[0].node.transform->rotation.y - 15) < .01f,
                "System failed to query typed Component");
        require(std::fabs(expanded.instances[1].expansion.nodes[1].node.transform->position.x - 10) < .01f,
                "Node behaviour did not receive its own node");
        const auto move = runtime.evaluate("import^ throne\nlet^ n = throne.find(\"Parent\")\nlet^ r = throne.runner(\"Late\")\nif^ n != nil^ and^ r != nil^ { n.setBehaviourRunner(0, r) }");
        require(move.text.empty(), move.text.c_str());
        log.clear();
        runtime.update(.5f);
        require(log == std::vector<std::string>{"system Update", "Child", "Parent", "Child", "Parent"},
                "Moving a behaviour repeated start or changed other instances");
        require(throne::ScriptRuntime::fields(root, "node.lh").size() == 1, "Node inspector field reflection failed");
        require(throne::ScriptRuntime::fields(root, "component.lh").size() == 2, "Component reflection failed");
        require(throne::ScriptRuntime::fields(root, "system.lh").empty(), "System reflection failed");

        write(root, "dual.lh", R"(module^ test.dual
import^ throne
public^let^ Component = def^{ self^{ data = 1 } }
public^let^ System = def^{ self^{ @export setting = 2 } }
public^let^ Behaviour = def^{ self^{ @export other = 3 } }
)");
        require(throne::ScriptRuntime::fields(root, "dual.lh", "Component")[0].name == "data" &&
                throne::ScriptRuntime::fields(root, "dual.lh", "System")[0].name == "setting" &&
                throne::ScriptRuntime::fields(root, "dual.lh", "Behaviour")[0].name == "other",
                "Inspector confused different exports of the same module");

        throne::Prefab prefab;
        prefab.addNode("Root").behaviours.push_back({"node.lh", {{"speed", 1.0}}});
        prefab.nodes[0].scriptComponents["Spin"] = {"component.lh", {{"speed", 1.0}}};
        library.put("prefab.lton", prefab);
        throne::Scene instances;
        auto& placed = instances.addNode("Instance");
        placed.prefab = "prefab.lton";
        placed.overrides.node({}).behaviours = std::vector<throne::Behaviour>{{"node.lh", {{"speed", 9.0}}, "Late"}};
        placed.overrides.node({}).scriptFields["Spin"]["speed"] = 8.0;
        instances.save(root / "instances.lton");
        auto resolved = throne::expand(throne::Scene::load(root / "instances.lton"), library);
        require(std::get<double>(resolved.nodes[0].node.scriptComponents.at("Spin").fields.at("speed")) == 8.0, "Prefab Component field override lost");
        require(resolved.nodes[0].node.behaviours[0].runner == "Late", "Prefab Behaviour override lost");
        placed.overrides.node({}).behaviours = std::vector<throne::Behaviour>{};
        resolved = throne::expand(instances, library);
        require(resolved.nodes[0].node.behaviours.empty(), "Prefab Behaviour removal failed");

        const auto mainThread = std::this_thread::get_id();
        for (const bool serial : {false, true}) {
            int completed = 0;
            std::mutex mutex;
            throne::runStages(throne::defaultStages(), serial, [&](const std::string& stage) {
                if (stage == "OGRE") require(std::this_thread::get_id() == mainThread, "OGRE left main thread");
                if (stage == "Box3D" || stage == "Animator") { std::lock_guard<std::mutex> lock(mutex); ++completed; }
                if (stage == "SyncSimulation") require(completed == 2, "Simulation barrier did not join workers");
            });
        }
        // Rendering sees the previous capture, and simulation results enter the following capture.
        std::vector<int> parallelFrames;
        for (const bool serial : {false, true}) {
            int node = 0, render = 0, next = 0, input = 0, result = 0;
            std::vector<int> drawn;
            for (int frame = 0; frame < 3; ++frame)
                throne::runStages(throne::defaultStages(), serial, [&](const std::string& stage) {
                    if (stage == "Update") node += 10;
                    if (stage == "SyncRender") input = node, next = node;
                    if (stage == "OGRE") drawn.push_back(render);
                    if (stage == "Box3D") result = input + 1;
                    if (stage == "SyncSimulation") node = result, render = next;
                });
            require(drawn == std::vector<int>{0, 10, 21}, "Frame snapshot latency changed");
            if (!serial) parallelFrames = drawn;
            else require(drawn == parallelFrames, "Serial execution changed snapshot timing");
        }
        // An exception on the main thread must still join both simulation drivers.
        int joined = 0;
        std::mutex joinedMutex;
        bool rendererFailed = false;
        try {
            throne::runStages(throne::defaultStages(), false, [&](const std::string& stage) {
                if (stage == "OGRE") throw std::runtime_error("renderer failure");
                if (stage == "Box3D" || stage == "Animator") {
                    std::lock_guard<std::mutex> lock(joinedMutex);
                    ++joined;
                }
            });
        } catch (const std::runtime_error&) { rendererFailed = true; }
        require(rendererFailed && joined == (std::thread::hardware_concurrency() < 3 ? 0 : 2),
                "A failed renderer left simulation work running");
        // Unknown stages and host-only groups must refuse node attachments before Play.
        auto rejectedWorld = world;
        rejectedWorld.scenes["scene.lton"].nodes[0].behaviours[0].runner = "Box3D";
        auto rejectedExpansion = throne::expand(rejectedWorld, library);
        bool refused = false;
        try {
            throne::ScriptRuntime bad(rejectedWorld, rejectedExpansion, root, [](const std::string&) {}, throne::PlayScope::Level);
        } catch (const throne::ScriptError& e) { refused = std::string(e.what()).find("Runner Box3D") != std::string::npos; }
        require(refused, "Node Behaviour accepted a parallel System as its Runner");
        // Parent-first storage is not necessarily depth-first storage.
        throne::Scene hierarchy;
        const auto a = hierarchy.addNode("A").id;
        hierarchy.addNode("B");
        hierarchy.addNode("A child", a);
        // Deliberately store the child after the other root.
        std::swap(hierarchy.nodes[1], hierarchy.nodes[2]);
        const auto ordered = throne::expand(hierarchy, library).hierarchy();
        require(ordered == std::vector<std::size_t>{0, 2, 1}, "Hierarchy execution followed flat storage order");
        // Node references may explicitly store no node.
        hierarchy.nodes[0].behaviours = {{"node.lh", {{"target", throne::NodeRef{}}}}};
        hierarchy.save(root / "nil-reference.lton");
        require(std::get<throne::NodeRef>(throne::Scene::load(root / "nil-reference.lton").nodes[0].behaviours[0].fields["target"]).node == 0,
                "An empty node reference failed to round-trip");
        auto invalid = throne::defaultStages();
        invalid.erase(invalid.begin());
        bool rejected = false;
        try { throne::validateStages(invalid); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Missing engine stage accepted");
        fs::remove_all(root);
        std::puts("Node behaviours, stages, script Components and Systems passed");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
