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

/// Returns the ScriptError message a session with @p script on the first object
/// raises while starting or within three frames, or an empty string.
std::string error(const fs::path& root, const char* script) {
    write(root, "scripts/broken.lh", script);
    auto scene = throne::SceneDocument::example();
    scene.objects[0].script = "scripts/broken.lh";
    try {
        throne::ScriptRuntime runtime(scene, root, [](const std::string&) {});
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
        speed = 90,
    },
    start = p^self^, obj:throne.Object {
        throne.log("start " .. obj.name())
    },
    update = p^self^, obj:throne.Object, dt:number^ {
        var^ r = obj.rotation()
        r.y := r.y + self^.speed * dt
        obj.setRotation(r)
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
    write(root, "scripts/main.lh", mainScript);

    auto scene = throne::SceneDocument::example();
    scene.objects[0].script = "scripts/spin.lh";
    std::vector<std::string> logged;
    try {
        throne::ScriptRuntime runtime(scene, root, [&](const std::string& line) { logged.push_back(line); });
        for (int i = 0; i < 10; ++i) runtime.update(0.1f);
    } catch (const throne::ScriptError& e) {
        return fail(std::string("Valid scripts failed:\n") + e.what());
    }
    if (std::abs(scene.objects[0].rotation.y - 90) > 0.01f)
        return fail("Behaviour.update did not rotate: y=" + std::to_string(scene.objects[0].rotation.y));
    if (std::abs(scene.objects[1].position.x - 3) > 0.01f)
        return fail("main.lh update did not move Cube 2: x=" + std::to_string(scene.objects[1].position.x));
    if (logged.size() != 1 || logged[0] != "start Cube 1") return fail("start was not logged exactly once");

    // Each case must be refused with a message naming the problem.
    fs::remove(root / "scripts/main.lh");
    const struct {
        const char *script, *expected;
    } failures[] = {
        {"import^ throne\nlet^ x = 1\n", "must declare module^"},
        {"module^ test.a\nlet^ x = 1\n", "must publish Behaviour"},
        {"module^ test.b\nimport^ throne\npublic^let^Behaviour = def^{\n"
         "    update = p^self^, obj:throne.Object { },\n}\n",
         "Behaviour.update must be"},
        {"module^ test.c\nimport^ throne\npublic^let^Behaviour = def^{\n"
         "    update = p^self^, obj:throne.Object, dt:number^ { repeat^while^ true^ { } },\n}\n",
         "per-frame budget"},
        {"module^ test.d\nimport^ throne\npublic^let^Behaviour = def^{\n"
         "    start = p^self^, obj:throne.Object { panic^ \"stop here\" },\n}\n",
         "stop here"},
        {"module^ test.e\npublic^let^Behaviour = def^{ oops }\n", "broken.lh"},
    };
    for (const auto& failure : failures) {
        const auto message = error(root, failure.script);
        if (message.find(failure.expected) == std::string::npos)
            return fail(std::string("Expected an error containing \"") + failure.expected + "\", got:\n" + message);
    }
    const auto api = throne::ScriptRuntime::hostApi();
    for (const char* name : {"\"throne\"", "Vec3", "Object", "setPosition", "math"})
        if (api.find(name) == std::string::npos) return fail(std::string("Host API JSON lacks ") + name);
    fs::remove_all(root);
    std::printf("Scripts ran 10 frames; %zu failure cases were reported\n", std::size(failures));
}
