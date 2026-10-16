#include "editor/SceneDocument.h"
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
/// Reports a failed check and returns the process exit code.
int fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return 1;
}
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() / "throne_scene_smoke";
    const auto path = directory / "roundtrip.throne.lton";
    auto original = throne::SceneDocument::example();
    original.objects.back().rotation = {0, 45, 10};
    original.objects.back().name = "Crate \xE2\x9C\x93";
    original.objects.back().script = "scripts/spin.lh";
    original.save(path);

    const auto loaded = throne::SceneDocument::load(path);
    if (loaded.objects.size() != original.objects.size() || loaded.nextId != original.nextId)
        return fail("Object count or next ID changed");
    for (size_t i = 0; i < loaded.objects.size(); ++i) {
        const auto& a = original.objects[i];
        const auto& b = loaded.objects[i];
        if (a.id != b.id || a.name != b.name || a.position != b.position || a.rotation != b.rotation ||
            a.scale != b.scale || a.colour != b.colour || a.script != b.script)
            return fail("Object changed after save/load");
    }

    const auto rejects = [&](const char* text) {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        try {
            throne::SceneDocument::load(path);
            return false;
        } catch (const std::runtime_error&) {
            return true;
        }
    };
    if (!rejects(R"(format = "ogrethrone-scene", version = 1, objects = {{ id = 1, name = "A", type = "cube",
                 position = {0, 0, 0}, rotation = {0, 0, 0}, scale = {0, 1, 1}, colour = {1, 1, 1} }})"))
        return fail("Zero scale was accepted");
    if (!rejects(R"(format = "ogrethrone-scene", version = 1, objects = {}, x = std.io.write("x"))"))
        return fail("A scene that calls a procedure was accepted");
    std::filesystem::remove_all(directory);
    std::printf("Scene save/load preserved %zu objects\n", loaded.objects.size());
}
