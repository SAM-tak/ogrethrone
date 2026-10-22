#include "editor/Project.h"
#include "script/ScriptRuntime.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
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
    // Scripts lie in the assets folder; main.lh in the project's root.
    const auto file = std::string_view(path) == "main.lh" ? root / path : root / "assets" / path;
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

/// A world and its expansion, as the editor hands them to a Play session.
struct Session {
    throne::World world;
    throne::LevelExpansion expanded;

    /// Nodes of the first scene instance.
    std::vector<throne::ExpandedNode>& nodes() { return expanded.instances[0].expansion.nodes; }
};

/// The example scene placed once, and one level system using @p script on its first node.
Session world(const fs::path& root, const char* script) {
    Session result;
    result.world.scenes["scenes/main.scene.lton"] = throne::Scene::example();
    const auto scene = result.world.level.addScene("scenes/main.scene.lton").id;
    auto& system = result.world.level.addSystem("Spinner");
    const auto cube = result.world.scenes["scenes/main.scene.lton"].nodes[0].id;
    system.behaviour.push_back({script, {{"target", throne::NodeRef{scene, cube, {}}}}});
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
        @export target : throne.SceneNode|nil^ = nil^,
        @export degreesPerSecond = 90,
        turns = 0,
    },
    start = p^self^, system:throne.System {
        throne.log("start " .. system.name())
    },
    update = p^self^, system:throne.System, dt:number^ {
        if^ self^.target != nil^ {
            let^ t = self^.target.transform()
            if^ t != nil^ {
                var^ r = t.rotation()
                r.y := r.y + self^.degreesPerSecond * dt
                t.setRotation(r)
            }
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
        let^ t = mover.transform()
        if^ t != nil^ { t.setPosition(t.position() + throne.vec3(1, 0, 0)) }
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
    auto& fields = played.world.level.systems[0].behaviour[0].data;
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
    if (std::abs(nodes[0].node.transform->rotation.y - 45) > 0.01f)
        return fail("Behaviour did not rotate its target node: y=" + std::to_string(nodes[0].node.transform->rotation.y));
    if (std::abs(nodes[1].node.transform->position.x - 5) > 0.01f)
        return fail("main.lh did not move Cube 2 once per frame for three frames: x=" +
                    std::to_string(nodes[1].node.transform->position.x));
    if (played.world.scenes.at("scenes/main.scene.lton").nodes[0].transform->rotation.y != 0)
        return fail("Play changed the world instead of its expansion");
    if (logged.size() != 2 || logged[0].find("field missing ignored") == std::string::npos ||
        logged[1] != "start Spinner")
        return fail("Expected a skipped-field warning and one start line");

    // Playing the level alone runs the systems but not main.lh.
    auto levelOnly = world(root, "scripts/spin.lh");
    try {
        throne::ScriptRuntime runtime(levelOnly.world, levelOnly.expanded, root, [](const std::string&) {},
                                      throne::PlayScope::Level);
        for (int i = 0; i < 10; ++i) runtime.update(0.1f);
    } catch (const throne::ScriptError& e) {
        return fail(std::string("Level play failed:\n") + e.what());
    }
    if (levelOnly.nodes()[1].node.transform->position.x != 2 || levelOnly.nodes()[0].node.transform->rotation.y < 89)
        return fail("Level play ran main.lh or skipped the systems");

    // The console runs lines on a session's machine: names stay, an input may span
    // lines, and a refused or failed one is reported without ending the session.
    {
        auto consoled = world(root, "scripts/spin.lh");
        std::vector<std::string> printed;
        throne::ScriptRuntime runtime(consoled.world, consoled.expanded, root,
                                      [&](const std::string& line) { printed.push_back(line); }, throne::PlayScope::Level);
        const auto bound = runtime.evaluate("let^ answer = 6 * 7");
        const auto read = runtime.evaluate("answer");
        const auto opened = runtime.evaluate("if^ answer > 40 {");
        const auto closed = runtime.evaluate("throne.log(\"big\") }");
        const auto found = runtime.evaluate("throne.find(\"Cube 2\").name()");
        const auto refused = runtime.evaluate("let^ = 1");
        const auto failed = runtime.evaluate("panic^ \"no\"");
        const auto after = runtime.evaluate("answer + 1");
        const auto shown = runtime.evaluate("print(\"answer\", answer, true^)");
        if (printed.empty() || printed.back() != "answer 42 true")
            return fail("print did not log its arguments separated by spaces: " +
                        (printed.empty() ? shown.text : printed.back()));
        if (bound.more || read.text != "42" || !opened.more || closed.more || found.text.find("Cube 2") == std::string::npos ||
            refused.text.empty() || failed.text.find("no") == std::string::npos || after.text != "43")
            return fail("The console did not run its lines as a session: " + read.text + " / " + found.text + " / " +
                        after.text);
    }

    // A prefab's system runs once per instance, each on the node inside its own instance.
    throne::Prefab spinner;
    const auto wheel = spinner.addNode("Wheel").id;
    spinner.addSystem("Spin").behaviour.push_back({"scripts/spin.lh", {{"target", throne::NodeRef{0, wheel, {}}}}});
    spinner.save(root / "prefabs/spinner.prefab.lton");
    Session prefabs;
    auto& placed = prefabs.world.scenes["scenes/two.scene.lton"];
    const auto a = placed.addNode("A").id, b = placed.addNode("B").id;
    for (const auto id : {a, b}) placed.findNode(id)->prefab = "prefabs/spinner.prefab.lton";
    placed.nodes[1].overrides.fieldsOf({}, 1, 0).fields["degreesPerSecond"] = 45.0;
    prefabs.world.level.addScene("scenes/two.scene.lton");
    throne::Library library(root);
    prefabs.expanded = throne::expand(prefabs.world, library);
    try {
        throne::ScriptRuntime runtime(prefabs.world, prefabs.expanded, root, [](const std::string&) {},
                                      throne::PlayScope::Level);
        for (int i = 0; i < 10; ++i) runtime.update(0.1f);
    } catch (const throne::ScriptError& e) {
        return fail(std::string("Prefab systems failed:\n") + e.what());
    }
    const auto& expansion = prefabs.expanded.instances[0].expansion;
    const auto& wheelA = expansion.nodes[expansion.find({a, wheel})].node;
    const auto& wheelB = expansion.nodes[expansion.find({b, wheel})].node;
    if (std::abs(wheelA.transform->rotation.y - 90) > 0.01f || std::abs(wheelB.transform->rotation.y - 45) > 0.01f)
        return fail("Prefab systems did not turn their own instance's wheel at their own speed");

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
         "    update = p^self^, system:throne.System { },\n}\n",
         "Behaviour.update must be"},
        {"module^ test.c\nimport^ throne\npublic^let^Behaviour = def^{\n"
         "    update = p^self^, system:throne.System, dt:number^ { repeat^while^ true^ { } },\n}\n",
         "per-frame budget"},
        {"module^ test.d\nimport^ throne\npublic^let^Behaviour = def^{\n"
         "    start = p^self^, system:throne.System { panic^ \"stop here\" },\n}\n",
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

    // Node Behaviour components retain independent script data, including two on one node.
    if (argc > 1) {
        const fs::path sample = argv[1];
        Session basic;
        throne::AssetDatabase assets(sample / "assets", {});
        assets.scan([](const std::string&) {});
        basic.world.level = throne::Level::load(sample / "assets/levels/main.level.lton");
        for (const auto& instance : basic.world.level.scenes)
            basic.world.scenes[instance.scene] = throne::Scene::load(sample / "assets" / fs::u8path(assets.path(instance.scene)));
        throne::Library library(sample / "assets", &assets);
        basic.expanded = throne::expand(basic.world, library);
        std::unique_ptr<throne::ScriptRuntime> runtime;
        try {
            runtime = std::make_unique<throne::ScriptRuntime>(basic.world, basic.expanded, sample, [](const std::string&) {},
                                                            throne::PlayScope::Level, &assets);
            for (int i = 0; i < 30; ++i) runtime->update(static_cast<float>(throne::fixedStep));
        } catch (const throne::ScriptError& e) {
            return fail(std::string("The sample was rejected:\n") + e.what());
        }
        if (std::fabs(basic.nodes()[0].node.transform->rotation.y - 22.5f) > 0.01f ||
            std::fabs(basic.nodes()[0].node.transform->rotation.x) > 0.01f ||
            std::fabs(basic.nodes()[1].node.transform->rotation.y - 22.5f) > 0.01f)
            return fail("The sample's Behaviour components lost their per-script data or failed to rotate");
        // The rotor post prefab spins its hub at 90 degrees a second; the scene's instance of the
        // fast variant overrides its 360 with 1360.
        std::vector<float> hubs;
        for (const auto& node : basic.nodes())
            if (node.node.name == "Hub") hubs.push_back(node.node.transform->rotation.y);
        if (hubs.size() != 2 || std::fabs(hubs[0] - 45.0f) > 0.01f || std::fabs(hubs[1] - 680.0f) > 0.01f)
            return fail("The sample's prefab and variant did not spin their hubs at their own speeds");
        const auto& position = basic.nodes()[0].node.transform->position;
        if (std::fabs(position.y - 0.5f) > 0.001f) return fail("Pop did not rest at its starting height between hops");
        // Updates are fixed 1/60 s steps; advance the sample by whole steps.
        const auto advance = [&](int steps) { while (steps--) runtime->update(static_cast<float>(throne::fixedStep)); };
        advance(72);
        if (std::fabs(position.y - 1.0f) > 0.001f) return fail("Pop did not reach its configured height");
        advance(90);
        if (std::fabs(position.y - 1.0f) > 0.001f) return fail("Pop did not repeat at its configured interval");
        advance(12);
        if (std::fabs(position.y - 0.5f) > 0.001f || position.x != 0 || position.z != 0)
            return fail("Pop failed to return to its starting position");
    }

    const auto api = throne::ScriptRuntime::hostApi();
    for (const char* name : {"\"throne\"", "Vec3", "SceneNode", "Transform", "BoxPrimitive", "System", "setPosition", "math"})
        if (api.find(name) == std::string::npos) return fail(std::string("Host API JSON lacks ") + name);
    fs::remove_all(root);
    std::printf("Scripts ran 10 frames; %zu failure cases were reported\n", std::size(failures));
}
