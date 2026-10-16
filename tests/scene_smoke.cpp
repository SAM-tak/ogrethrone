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
    const auto path = directory / "roundtrip.throne.json";
    auto original = throne::SceneDocument::example();
    original.objects.back().rotation = {0, 45, 10};
    original.objects.back().name = "Crate \xE2\x9C\x93";
    original.save(path);

    const auto loaded = throne::SceneDocument::load(path);
    if (loaded.objects.size() != original.objects.size() || loaded.nextId != original.nextId)
        return fail("Object count or next ID changed");
    for (size_t i = 0; i < loaded.objects.size(); ++i) {
        const auto& a = original.objects[i];
        const auto& b = loaded.objects[i];
        if (a.id != b.id || a.name != b.name || a.position != b.position || a.rotation != b.rotation ||
            a.scale != b.scale || a.colour != b.colour)
            return fail("Object changed after save/load");
    }

    std::ofstream(path, std::ios::trunc) << R"({"format":"ogrethrone-scene","version":1,"objects":[)"
                                         << R"({"id":1,"name":"A","type":"cube","position":[0,0,0],)"
                                         << R"("rotation":[0,0,0],"scale":[0,1,1],"colour":[1,1,1]}]})";
    try {
        throne::SceneDocument::load(path);
        return fail("Zero scale was accepted");
    } catch (const std::runtime_error&) {
    }
    std::filesystem::remove_all(directory);
    std::printf("Scene save/load preserved %zu objects\n", loaded.objects.size());
}
