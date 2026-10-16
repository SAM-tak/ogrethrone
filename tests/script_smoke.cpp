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

/// A world with the example scene and one level runner using @p script.
throne::World world(const char* script) {
    throne::World result;
    result.scenes.push_back({"scenes/main.scene.lton", throne::Scene::example()});
    auto& runner = result.level.addRunner("Spinner");
    runner.behaviours.push_back({script, {{"target", throne::NodeRef{"scenes/main.scene.lton", 1}}}});
    return result;
}

/// Returns the ScriptError message a session with @p script raises while
/// starting or within three frames, or an empty string.
std::string error(const fs::path& root, const char* script) {
    write(root, "scripts/broken.lh", script);
    auto broken = world("scripts/broken.lh");
    try {
        throne::ScriptRuntime runtime(broken, root, [](const std::string&) {});
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

constexpr const char* mainScript = R"(module^ test.main
import^ throne

public^let^update = p^dt:number^ {
    let^ mover = throne.find("Cube 2")
    if^ mover != nil^ {
        mover.setPosition(mover.position() + throne.vec3(dt, 0, 0))
    }
}
)";
}

int main() {
    const auto root = fs::temp_directory_path() / "throne_script_smoke";
    fs::remove_all(root);
    write(root, "scripts/spin.lh", spin);
    write(root, "main.lh", mainScript);

    // A field value overrides the default; a field the script lacks is reported and skipped.
    auto played = world("scripts/spin.lh");
    auto& fields = played.level.runners[0].behaviours[0].fields;
    fields["degreesPerSecond"] = 45.0;
    fields["missing"] = 1.0;
    std::vector<std::string> logged;
    try {
        throne::ScriptRuntime runtime(played, root, [&](const std::string& line) { logged.push_back(line); });
        for (int i = 0; i < 10; ++i) runtime.update(0.1f);
    } catch (const throne::ScriptError& e) {
        return fail(std::string("Valid scripts failed:\n") + e.what());
    }
    const auto& nodes = played.scenes[0].scene.nodes;
    if (std::abs(nodes[0].rotation.y - 45) > 0.01f)
        return fail("Behaviour did not rotate its target node: y=" + std::to_string(nodes[0].rotation.y));
    if (std::abs(nodes[1].position.x - 3) > 0.01f)
        return fail("main.lh update did not move Cube 2: x=" + std::to_string(nodes[1].position.x));
    if (logged.size() != 2 || logged[0].find("field missing ignored") == std::string::npos ||
        logged[1] != "start Spinner")
        return fail("Expected a skipped-field warning and one start line");

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
        throne::ScriptRuntime runtime(empty, project.root, [](const std::string&) {});
        runtime.update(0.1f);
    } catch (const throne::ScriptError& e) {
        return fail(std::string("Generated main.lh was rejected:\n") + e.what());
    }

    const auto api = throne::ScriptRuntime::hostApi();
    for (const char* name : {"\"throne\"", "Vec3", "SceneNode", "Runner", "setPosition", "math"})
        if (api.find(name) == std::string::npos) return fail(std::string("Host API JSON lacks ") + name);
    fs::remove_all(root);
    std::printf("Scripts ran 10 frames; %zu failure cases were reported\n", std::size(failures));
}
