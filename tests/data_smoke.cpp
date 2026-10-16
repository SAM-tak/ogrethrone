#include "editor/Level.h"
#include "editor/Project.h"
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
    const auto directory = std::filesystem::temp_directory_path() / "throne_data_smoke";
    std::filesystem::remove_all(directory);
    const auto path = directory / "roundtrip.level.lton";
    auto original = throne::Level::example();
    original.objects.back().rotation = {0, 45, 10};
    original.objects.back().name = "Crate \xE2\x9C\x93";
    original.objects.back().script = "scripts/spin.lh";
    original.save(path);

    const auto loaded = throne::Level::load(path);
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
            throne::Level::load(path);
            return false;
        } catch (const std::runtime_error&) {
            return true;
        }
    };
    if (!rejects(R"(format = "ogrethrone-level", version = 1, objects = {{ id = 1, name = "A", type = "cube",
                 position = {0, 0, 0}, rotation = {0, 0, 0}, scale = {0, 1, 1}, colour = {1, 1, 1} }})"))
        return fail("Zero scale was accepted");
    if (!rejects(R"(format = "ogrethrone-level", version = 1, objects = {}, x = std.io.write("x"))"))
        return fail("A level that calls a procedure was accepted");

    // A new project reopens with its main level, and the project list round-trips.
    const auto project = throne::Project::create(directory, "My Game");
    const auto reopened = throne::Project::open(project.root / throne::Project::markerName);
    if (reopened.name != "My Game" || reopened.mainLevel != "levels/main.level.lton" ||
        throne::Level::load(reopened.resolve(reopened.mainLevel)).objects.size() != 2)
        return fail("Created project did not reopen with its main level");
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

    std::filesystem::remove_all(directory);
    std::printf("Level save/load preserved %zu objects; project and list round-tripped\n", loaded.objects.size());
}
