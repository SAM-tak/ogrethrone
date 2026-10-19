#include "script/ScriptRuntime.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <limits>

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
        write(root, "system.lh", R"(module^ test.system
import^ throne
public^let^ Behaviour = def^{
    update = p^self^, system:throne.System, dt:number^ {
        throne.log("system " .. system.name())
        for^ _^, node in^ throne.query("Transform", "BoxPrimitive") {
            let^ t = node.component(throne.Transform)
            if^ t != nil^ { t.setRotation(t.rotation() + throne.vec3(0, 30 * dt, 0)) }
        }
    },
}
)");
        throne::World world;
        auto& scene = world.scenes["scene.lton"];
        const auto parent = scene.addNode("Parent").id;
        scene.addNode("Child", parent).behaviour.push_back({"node.lh", {{"speed", 20.0}}});
        scene.nodes[0].behaviour.push_back({"node.lh", {}});
        world.level.addScene("scene.lton");
        world.level.addScene("scene.lton");
        world.level.addSystem("Update").behaviour.push_back({"system.lh", {}});
        world.level.stages.insert(world.level.stages.begin() + 3, {"Late", {}});
        scene.save(root / "scene.lton");
        world.level.save(root / "level.lton");
        world.scenes["scene.lton"] = throne::Scene::load(root / "scene.lton");
        world.level = throne::Level::load(root / "level.lton");
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
                "System failed to query built-in components");
        require(std::fabs(expanded.instances[1].expansion.nodes[1].node.transform->position.x - 10) < .01f,
                "Node behaviour did not receive its own node");
        const auto move = runtime.evaluate("import^ throne\nlet^ n = throne.find(\"Parent\")\nlet^ r = throne.system(\"Late\")\nif^ n != nil^ and^ r != nil^ { n.setBehaviourPriority(0, 2) }");
        require(move.text.empty(), move.text.c_str());
        log.clear();
        runtime.update(.5f);
        require(log == std::vector<std::string>{"system Update", "Child", "Parent", "Child", "Parent"},
                "Moving a behaviour repeated start or changed other instances");
        require(throne::ScriptRuntime::fields(root, "node.lh").size() == 1, "Node inspector field reflection failed");
        require(throne::ScriptRuntime::fields(root, "system.lh").empty(), "System reflection failed");
        // Type arguments retain concrete return types and dispatch by native type identity.
        const auto typed = runtime.evaluate(R"(import^ throne
let^ n = throne.find("Parent")
if^ n != nil^ {
    let^ kind = throne.Transform
    let^ t:throne.Transform|nil^ = n.component(kind)
    let^ b:throne.BoxPrimitive|nil^ = n.component(throne.BoxPrimitive)
    if^ t != nil^ { t.setScale(throne.vec3(2, 3, 4)) }
    if^ b != nil^ { b.setColour(throne.vec3(0.2, 0.3, 0.4)) }
}
)");
        require(typed.text.empty(), typed.text.c_str());
        require(expanded.instances[0].expansion.nodes[0].node.transform->scale.y == 3,
                "Typed component access did not edit the native component");
        require(std::fabs(expanded.instances[0].expansion.nodes[0].node.box->colour[1] - .3f) < .001f,
                "Typed BoxPrimitive access returned the wrong native component");
        for (const auto type : {"SpherePrimitive", "CapsulePrimitive", "PlanePrimitive", "BoxCollider",
                                "SphereCollider", "CapsuleCollider", "PlaneCollider", "RigidBody",
                                "PbsMaterial", "Light", "ParticleSystem"}) {
            const auto before = log.size();
            const auto absent = runtime.evaluate(std::string("import^ throne\nlet^ n = throne.find(\"Parent\")\n") +
                "if^ n != nil^ { let^ c:throne." + type + "|nil^ = n.component(throne." + type +
                ")\nif^ c != nil^ { throne.log(\"unexpected component\") } }");
            require(absent.text.empty(), absent.text.c_str());
            require(log.size() == before, "Missing component did not return nil");
        }
        const auto mismatch = runtime.evaluate(R"(import^ throne
let^ n = throne.find("Parent")
if^ n != nil^ { let^ t:throne.BoxPrimitive|nil^ = n.component(throne.Transform) }
)");
        require(!mismatch.text.empty(), "component(type) erased its concrete return type");
        for (const auto type : {"throne.SceneNode", "throne.Vec3", "number^"}) {
            const auto missing = runtime.evaluate(std::string("import^ throne\nlet^ n = throne.find(\"Parent\")\n") +
                "if^ n != nil^ { let^ c = n.component(" + type + ") }");
            require(!missing.text.empty(), "Unsupported component type was accepted");
        }
        for (const auto symbol : {"Component", "System"}) {
            write(root, "definition.lh", (std::string("module^ test.definition\npublic^let^ ") + symbol +
                " = def^{ self^{ hp = 100 } }\n").c_str());
            for (const auto requested : {"", symbol}) {
                bool rejected = false;
                try { throne::ScriptRuntime::fields(root, "definition.lh", requested); }
                catch (const throne::ScriptError&) { rejected = true; }
                require(rejected, "Component/System script definition was accepted by reflection");
            }
        }

        throne::Prefab prefab;
        prefab.addNode("Root").behaviour.push_back({"node.lh", {{"speed", 1.0}}});
        library.put("prefab.lton", prefab);
        throne::Scene instances;
        auto& placed = instances.addNode("Instance");
        placed.prefab = "prefab.lton";
        placed.overrides.node({}).behaviour = std::vector<throne::Behaviour>{{"node.lh", {{"speed", 9.0}}}}
        ; placed.overrides.node({}).behaviour->front().priority=3.0;
        instances.save(root / "instances.lton");
        auto resolved = throne::expand(throne::Scene::load(root / "instances.lton"), library);
        require(resolved.nodes[0].node.behaviour[0].priority == 3.0, "Prefab Behaviour override lost");
        placed.overrides.node({}).behaviour = std::vector<throne::Behaviour>{};
        resolved = throne::expand(instances, library);
        require(resolved.nodes[0].node.behaviour.empty(), "Prefab Behaviour removal failed");

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
                    if (stage == "UpdateBehaviourSystem") node += 10;
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
        // Non-finite ordering values must be refused before sorting any query.
        auto rejectedWorld = world;
        rejectedWorld.scenes["scene.lton"].nodes[0].behaviour[0].priority = std::numeric_limits<double>::infinity();
        auto rejectedExpansion = throne::expand(rejectedWorld, library);
        bool refused = false;
        try {
            throne::ScriptRuntime bad(rejectedWorld, rejectedExpansion, root, [](const std::string&) {}, throne::PlayScope::Level);
        } catch (const throne::ScriptError& e) { refused = std::string(e.what()).find("Priority must be finite") != std::string::npos; }
        require(refused, "Node Behaviour accepted a non-finite priority");
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
        hierarchy.nodes[0].behaviour = {{"node.lh", {{"target", throne::NodeRef{}}}}};
        hierarchy.save(root / "nil-reference.lton");
        require(std::get<throne::NodeRef>(throne::Scene::load(root / "nil-reference.lton").nodes[0].behaviour[0].data["target"]).node == 0,
                "An empty node reference failed to round-trip");
        auto invalid = throne::defaultStages();
        invalid.erase(invalid.begin());
        bool rejected = false;
        try { throne::validateStages(invalid); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Missing engine stage accepted");
        fs::remove_all(root);
        std::puts("Node behaviour, stages, built-in component queries and Systems passed");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
