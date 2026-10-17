#include "editor/Project.h"
#include "script/ScriptRuntime.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

/// Reports a failed check and returns the process exit code.
int fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    return 1;
}

/// Writes @p text to @p root / @p path, creating directories.
void write(const fs::path& root, const char* path, const char* text) {
    fs::create_directories((root / path).parent_path());
    std::ofstream(root / path, std::ios::binary | std::ios::trunc) << text;
}

/// A world and its expansion, as the editor hands them to a Play session.
struct Session {
    throne::World world;
    throne::LevelExpansion expanded;

    /// Nodes of the first scene instance.
    std::vector<throne::ExpandedNode>& nodes() { return expanded.instances[0].expansion.nodes; }
};

/// The example scene placed once, and one level runner using @p script on its first node.
Session world(const fs::path& root, const char* script) {
    Session result;
    result.world.scenes["scenes/main.scene.lton"] = throne::Scene::example();
    const auto scene = result.world.level.addScene("scenes/main.scene.lton").id;
    auto& runner = result.world.level.addRunner("Spinner");
    runner.behaviours.push_back({script, {{"target", throne::NodeRef{scene, 1, {}}}}});
    throne::Library library(root);
    result.expanded = throne::expand(result.world, library);
    return result;
}

/// Returns the ScriptError message a session with @p script raises while
/// starting or within three frames, or an empty string.
std::string error(const fs::path& root, const char* script) {
    write(root, "scripts/broken.lh", script);
    auto broken = world(root, "scripts/broken.lh");
    try {
        throne::ScriptRuntime runtime(broken.world, broken.expanded, root, [](const std::string&) {},
                                      throne::PlayScope::Level);
        for (int i = 0; i < 3; ++i) runtime.update(0.1f);
    } catch (const throne::ScriptError& e) {
        return e.what();
    }
    return {};
}

constexpr const char* spin = R"(module^ test.spin
import^ throne

public^let^Behaviour = def^{
    self^{
        target : throne.SceneNode|nil^ = nil^,
        degreesPerSecond = 90,
    },
    start = p^self^, runner:throne.Runner {
        throne.log("start " .. runner.name())
    },
    update = p^self^, runner:throne.Runner, dt:number^ {
        if^ self^.target != nil^ {
            var^ r = self^.target.rotation()
            r.y := r.y + self^.degreesPerSecond * dt
            self^.target.setRotation(r)
        }
    },
}
)";

// A coroutine script: moves Cube 2 one unit per frame for three frames, then ends.
constexpr const char* mainScript = R"(import^ throne

let^ nextFrame = p^ {
    yield^
}

var^ moved = 0
repeat^while^ moved < 3 {
    let^ mover = throne.find("Cube 2")
    if^ mover != nil^ {
        mover.setPosition(mover.position() + throne.vec3(1, 0, 0))
    }
    await^ nextFrame()
    moved := moved + 1
}
)";
}

int main(int argc, char** argv) {
    const auto root = fs::temp_directory_path() / "throne_script_smoke";
    fs::remove_all(root);
    write(root, "scripts/spin.lh", spin);
    write(root, "main.lh", mainScript);

    // A field value overrides the default; a field the script lacks is reported and skipped.
    auto played = world(root, "scripts/spin.lh");
    auto& fields = played.world.level.runners[0].behaviours[0].fields;
    fields["degreesPerSecond"] = 45.0;
    fields["missing"] = 1.0;
    std::vector<std::string> logged;
    try {
        throne::ScriptRuntime runtime(played.world, played.expanded, root,
                                      [&](const std::string& line) { logged.push_back(line); }, throne::PlayScope::Game);
        for (int i = 0; i < 10; ++i) runtime.update(0.1f);
        if (!runtime.finished()) return fail("main.lh's coroutine did not finish");
    } catch (const throne::ScriptError& e) {
        return fail(std::string("Valid scripts failed:\n") + e.what());
    }
    const auto& nodes = played.nodes();
    if (std::abs(nodes[0].node.rotation.y - 45) > 0.01f)
        return fail("Behaviour did not rotate its target node: y=" + std::to_string(nodes[0].node.rotation.y));
    if (std::abs(nodes[1].node.position.x - 5) > 0.01f)
        return fail("main.lh did not move Cube 2 once per frame for three frames: x=" +
                    std::to_string(nodes[1].node.position.x));
    if (played.world.scenes.at("scenes/main.scene.lton").nodes[0].rotation.y != 0)
        return fail("Play changed the world instead of its expansion");
    if (logged.size() != 2 || logged[0].find("field missing ignored") == std::string::npos ||
        logged[1] != "start Spinner")
        return fail("Expected a skipped-field warning and one start line");

    // Playing the level alone runs the runners but not main.lh.
    auto levelOnly = world(root, "scripts/spin.lh");
    try {
        throne::ScriptRuntime runtime(levelOnly.world, levelOnly.expanded, root, [](const std::string&) {},
                                      throne::PlayScope::Level);
        for (int i = 0; i < 10; ++i) runtime.update(0.1f);
    } catch (const throne::ScriptError& e) {
        return fail(std::string("Level play failed:\n") + e.what());
    }
    if (levelOnly.nodes()[1].node.position.x != 2 || levelOnly.nodes()[0].node.rotation.y < 89)
        return fail("Level play ran main.lh or skipped the runners");

    // A prefab's runner runs once per instance, each on the node inside its own instance.
    throne::Prefab spinner;
    spinner.addNode("Wheel");
    spinner.addRunner("Spin").behaviours.push_back({"scripts/spin.lh", {{"target", throne::NodeRef{0, 1, {}}}}});
    spinner.save(root / "prefabs/spinner.prefab.lton");
    Session prefabs;
    auto& placed = prefabs.world.scenes["scenes/two.scene.lton"];
    placed.addNode("A").prefab = "prefabs/spinner.prefab.lton";
    placed.addNode("B").prefab = "prefabs/spinner.prefab.lton";
    placed.nodes[1].overrides.fieldsOf({}, 1, 0).fields["degreesPerSecond"] = 45.0;
    prefabs.world.level.addScene("scenes/two.scene.lton");
    throne::Library library(root);
    prefabs.expanded = throne::expand(prefabs.world, library);
    try {
        throne::ScriptRuntime runtime(prefabs.world, prefabs.expanded, root, [](const std::string&) {},
                                      throne::PlayScope::Level);
        for (int i = 0; i < 10; ++i) runtime.update(0.1f);
    } catch (const throne::ScriptError& e) {
        return fail(std::string("Prefab runners failed:\n") + e.what());
    }
    const auto& expansion = prefabs.expanded.instances[0].expansion;
    const auto& wheelA = expansion.nodes[expansion.find({1, 1})].node;
    const auto& wheelB = expansion.nodes[expansion.find({2, 1})].node;
    if (std::abs(wheelA.rotation.y - 90) > 0.01f || std::abs(wheelB.rotation.y - 45) > 0.01f)
        return fail("Prefab runners did not turn their own instance's wheel at their own speed");

    const auto described = throne::ScriptRuntime::fields(root, "scripts/spin.lh");
    if (described.size() != 2 || described[0].name != "degreesPerSecond" ||
        described[0].kind != throne::FieldKind::Number || std::get<double>(described[0].defaultValue) != 90 ||
        described[1].name != "target" || described[1].kind != throne::FieldKind::SceneNode)
        return fail("Field description did not list degreesPerSecond and target");

    // Each case must be refused with a message naming the problem.
    fs::remove(root / "main.lh");
    const struct {
        const char *script, *expected;
    } failures[] = {
        {"import^ throne\nlet^ x = 1\n", "must declare module^"},
        {"module^ test.a\nlet^ x = 1\n", "must publish Behaviour"},
        {"module^ test.b\nimport^ throne\npublic^let^Behaviour = def^{\n"
         "    update = p^self^, runner:throne.Runner { },\n}\n",
         "Behaviour.update must be"},
        {"module^ test.c\nimport^ throne\npublic^let^Behaviour = def^{\n"
         "    update = p^self^, runner:throne.Runner, dt:number^ { repeat^while^ true^ { } },\n}\n",
         "per-frame budget"},
        {"module^ test.d\nimport^ throne\npublic^let^Behaviour = def^{\n"
         "    start = p^self^, runner:throne.Runner { panic^ \"stop here\" },\n}\n",
         "stop here"},
        {"module^ test.e\npublic^let^Behaviour = def^{ oops }\n", "broken.lh"},
    };
    for (const auto& failure : failures) {
        const auto message = error(root, failure.script);
        if (message.find(failure.expected) == std::string::npos)
            return fail(std::string("Expected an error containing \"") + failure.expected + "\", got:\n" + message);
    }

    // A new project's generated main.lh passes the checks and runs.
    const auto project = throne::Project::create(root, "Fresh");
    try {
        throne::World empty;
        throne::LevelExpansion nothing;
        throne::ScriptRuntime runtime(empty, nothing, project.root, [](const std::string&) {}, throne::PlayScope::Game);
        runtime.update(0.1f);
    } catch (const throne::ScriptError& e) {
        return fail(std::string("Generated main.lh was rejected:\n") + e.what());
    }

    // The bundled sample loads and its scene runner spins the scene's own cube.
    if (argc > 1) {
        const fs::path sample = argv[1];
        Session basic;
        basic.world.level = throne::Level::load(sample / "levels/main.level.lton");
        for (const auto& instance : basic.world.level.scenes)
            basic.world.scenes[instance.scene] = throne::Scene::load(sample / instance.scene);
        throne::Library library(sample);
        basic.expanded = throne::expand(basic.world, library);
        try {
            throne::ScriptRuntime runtime(basic.world, basic.expanded, sample, [](const std::string&) {},
                                          throne::PlayScope::Level);
            runtime.update(0.5f);
        } catch (const throne::ScriptError& e) {
            return fail(std::string("The sample was rejected:\n") + e.what());
        }
        if (std::fabs(basic.nodes()[0].node.rotation.y - 45.0f) > 0.01f)
            return fail("The sample's runner did not spin its cube");
    }

    const auto api = throne::ScriptRuntime::hostApi();
    for (const char* name : {"\"throne\"", "Vec3", "SceneNode", "Runner", "setPosition", "math"})
        if (api.find(name) == std::string::npos) return fail(std::string("Host API JSON lacks ") + name);
    fs::remove_all(root);
    std::printf("Scripts ran 10 frames; %zu failure cases were reported\n", std::size(failures));
}
