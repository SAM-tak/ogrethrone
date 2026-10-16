#include "editor/Project.h"
#include "editor/Level.h"
#include "editor/Lton.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>

namespace throne {
namespace {
namespace fs = std::filesystem;
constexpr std::string_view projectFormat = "ogrethrone-project", listFormat = "ogrethrone-projects";
constexpr LhatInteger projectVersion = 1, listVersion = 1;
constexpr const char* mainLevelPath = "levels/main.level.lton";

/// The entry point a new project starts with (TDDja/03 3.2).
constexpr std::string_view mainScript = R"(# The game's entry point: runs before every runner in each frame of Play.
module^ main
import^ throne

public^let^start = p^{
}

public^let^update = p^dt:number^{
}
)";

/// Whether two folders are the same, comparing their normalized absolute forms.
bool same(const fs::path& a, const fs::path& b) {
    std::error_code error;
    return fs::weakly_canonical(a, error) == fs::weakly_canonical(b, error);
}
}

Project Project::create(const fs::path& parent, const std::string& name) {
    if (name.empty() || name.size() > 100 || name.find_first_of("<>:\"/\\|?*") != std::string::npos ||
        name.front() == '.' || name.back() == ' ' || name.back() == '.')
        throw std::runtime_error("Project names must be 1-100 characters without <>:\"/\\|?* or edge dots");
    const auto root = fs::absolute(parent) / fs::u8path(name);
    if (fs::exists(root) && !fs::is_empty(root)) throw std::runtime_error(root.u8string() + " is not empty");
    fs::create_directories(root / "scripts");
    Project project{root, name, mainLevelPath};
    lton::writeAtomically(project.resolve("main.lh"), mainScript);
    Scene::example().save(project.resolve("scenes/main.scene.lton"));
    Level level;
    level.scenes.push_back("scenes/main.scene.lton");
    level.save(project.resolve(mainLevelPath));
    project.save();
    return project;
}

Project Project::open(const fs::path& path) {
    const auto root = fs::absolute(path.filename() == markerName ? path.parent_path() : path).lexically_normal();
    const lton::Document document(root / markerName);
    document.expect(projectFormat, projectVersion);
    Project project{root, std::string(lton::string(lton::field(document.root(), "name"))), {}};
    if (const auto level = lton::field(document.root(), "mainLevel"); !lhat_is_nil(level))
        project.mainLevel = lton::relativePath(level);
    return project;
}

void Project::save() const {
    lton::Writer write;
    const auto root = write.table();
    write.set(root, "format", write.string(projectFormat));
    write.set(root, "version", lhat_integer(projectVersion));
    write.set(root, "name", write.string(name));
    if (!mainLevel.empty()) write.set(root, "mainLevel", write.string(mainLevel));
    write.save(root, this->root / markerName);
}

fs::path Project::resolve(std::string_view relative) const { return root / fs::u8path(relative); }

fs::path ProjectList::defaultFile() {
#ifdef _WIN32
    if (const wchar_t* appData = _wgetenv(L"APPDATA")) return fs::path(appData) / "OgreThrone" / "projects.lton";
#else
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
        return fs::path(config) / "ogrethrone" / "projects.lton";
    if (const char* home = std::getenv("HOME")) return fs::path(home) / ".config" / "ogrethrone" / "projects.lton";
#endif
    return "projects.lton";
}

ProjectList ProjectList::load(const fs::path& file) {
    ProjectList list;
    if (!fs::exists(file)) return list;
    try {
        const lton::Document document(file);
        document.expect(listFormat, listVersion);
        const auto& projects = lton::table(lton::field(document.root(), "projects"));
        for (std::size_t i = 0, count = lton::sequence(projects); i < count; ++i) {
            const auto& entry = lton::table(lton::at(projects, i));
            list.entries.push_back({fs::u8path(lton::string(lton::field(entry, "path"))),
                                    lton::integer(lton::field(entry, "lastOpened"))});
        }
    } catch (const std::exception&) {
        // A damaged list is not worth refusing to start over; it is rewritten on the next change.
        list.entries.clear();
    }
    return list;
}

void ProjectList::save(const fs::path& file) const {
    lton::Writer write;
    const auto root = write.table();
    write.set(root, "format", write.string(listFormat));
    write.set(root, "version", lhat_integer(listVersion));
    const auto projects = write.table();
    for (const auto& entry : entries) {
        const auto item = write.table();
        write.set(item, "path", write.string(entry.root.generic_u8string()));
        write.set(item, "lastOpened", lhat_integer(entry.lastOpened));
        write.push(projects, item);
    }
    write.set(root, "projects", projects);
    write.save(root, file);
}

void ProjectList::touch(const fs::path& root) {
    remove(root);
    entries.insert(entries.begin(), {root, static_cast<std::int64_t>(std::time(nullptr))});
}

void ProjectList::remove(const fs::path& root) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& entry) { return same(entry.root, root); }),
                  entries.end());
}
}
