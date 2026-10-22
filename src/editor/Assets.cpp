#include "editor/Assets.h"
#include "audio/Mixer.h"
#include "core/Ids.h"
#include "editor/Level.h"
#include "editor/Lton.h"
#include "editor/Scene.h"
#include <algorithm>
#include <set>
#include <vector>

namespace throne {
namespace {
namespace fs = std::filesystem;
constexpr std::string_view metaFormat = "ogrethrone-meta", indexFormat = "ogrethrone-asset-index";
constexpr LhatInteger metaVersion = 1, indexVersion = 1;

/// Whether @p text ends with @p suffix.
bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// The modification time of @p file as an integer, or -1 when it cannot be read.
LhatInteger stamp(const fs::path& file) {
    std::error_code error;
    const auto time = fs::last_write_time(file, error);
    return error ? -1 : static_cast<LhatInteger>(time.time_since_epoch().count());
}

AssetId readId(LhatValue value) { return lton::assetId(value); }
}

AssetId newAssetId() {
    // Counting from 2026-10-01 UTC; its own sequence, apart from the nodes'.
    static Ids ids(1790812800000);
    return ids.next();
}

namespace lton {
AssetRef assetRef(LhatValue value) {
    if (lhat_is_nil(value)) return {};
    if (lhat_is_integer(value)) return AssetRef::of(readId(value));
    return AssetRef(relativePath(value));
}

LhatValue assetRef(Writer& out, const AssetRef& ref) {
    return ref.id ? lhat_integer(static_cast<LhatInteger>(ref.id)) : out.string(ref.path);
}

AssetId assetId(LhatValue value) {
    const auto id = integer(value);
    if (id <= 0) invalid();
    return static_cast<AssetId>(id);
}
}

AssetDatabase::AssetDatabase(fs::path assets, fs::path index) : root_(std::move(assets)), index_(std::move(index)) {}

bool AssetDatabase::managed(const std::string& path) {
    return endsWith(path, ".level.lton") || endsWith(path, ".scene.lton") || endsWith(path, ".prefab.lton") ||
           endsWith(path, ".mixer.lton");
}

void AssetDatabase::scan(const Log& log) {
    // The previous scan: each file's ID, and when the file holding it was written.
    std::map<std::string, std::pair<AssetId, LhatInteger>> cached;
    if (!index_.empty() && fs::exists(index_)) {
        try {
            const lton::Document document(index_);
            document.expect(indexFormat, indexVersion);
            const auto& list = lton::table(lton::field(document.root(), "assets"));
            for (std::size_t i = 0, n = lton::sequence(list); i < n; ++i) {
                const auto& item = lton::table(lton::at(list, i));
                cached[std::string(lton::string(lton::field(item, "path")))] = {readId(lton::field(item, "id")),
                                                                               lton::integer(lton::field(item, "time"))};
            }
        } catch (const std::exception&) {
            cached.clear();
        }
    }
    std::vector<std::pair<std::string, AssetId>> found;
    std::set<std::string> metas;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(root_, fs::directory_options::skip_permission_denied, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        const auto name = it->path().filename().u8string();
        // Hidden files and folders, and the old GI bakes beside scenes, are not assets.
        if (!name.empty() && name.front() == '.') {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory()) {
            if (endsWith(name, ".gi")) it.disable_recursion_pending();
            continue;
        }
        const auto path = it->path().lexically_relative(root_).generic_u8string();
        if (endsWith(name, ".meta")) {
            metas.insert(path.substr(0, path.size() - 5));
            continue;
        }
        if (endsWith(name, ".tmp")) continue;
        // Unchanged since the last scan: its ID is the one recorded.
        const auto entry = cached.find(path);
        const bool unchanged = entry != cached.end() && entry->second.second >= 0 &&
                               entry->second.second == stamp(root_ / fs::u8path(managed(path) ? path : path + ".meta"));
        found.emplace_back(path, unchanged ? entry->second.first : read(path, log));
    }
    for (const auto& meta : metas)
        if (!std::any_of(found.begin(), found.end(), [&](const auto& each) { return each.first == meta; }))
            log("Asset metadata without its file: " + meta + ".meta");
    // Files without an ID get one; of files sharing one, the one that had it last time keeps it.
    std::map<AssetId, std::vector<std::size_t>> holders;
    for (std::size_t i = 0; i < found.size(); ++i) holders[found[i].second].push_back(i);
    for (auto& [id, files] : holders) {
        std::vector<std::size_t> renew;
        if (!id) {
            renew = files;
        } else if (files.size() > 1) {
            auto keeper = files.front();
            for (const auto i : files)
                if (const auto entry = cached.find(found[i].first); entry != cached.end() && entry->second.first == id) keeper = i;
            for (const auto i : files)
                if (i != keeper) renew.push_back(i);
        }
        for (const auto i : renew) {
            const auto made = newAssetId();
            try {
                write(found[i].first, made);
                if (id) log("Asset " + found[i].first + " had the ID of " + found[files.front()].first + "; it has a new one");
                found[i].second = made;
            } catch (const std::exception& failure) {
                log("Could not give " + found[i].first + " an ID: " + failure.what());
            }
        }
    }
    byPath_.clear();
    byId_.clear();
    for (const auto& [path, id] : found) {
        if (!id) continue;
        byPath_[path] = id;
        byId_[id] = path;
    }
    if (index_.empty()) return;
    try {
        lton::Writer out;
        const auto root = out.table(), list = out.table();
        out.set(root, "format", out.string(indexFormat));
        out.set(root, "version", lhat_integer(indexVersion));
        for (const auto& [path, id] : byPath_) {
            const auto item = out.table();
            out.set(item, "path", out.string(path));
            out.set(item, "id", lhat_integer(static_cast<LhatInteger>(id)));
            out.set(item, "time", lhat_integer(stamp(root_ / fs::u8path(managed(path) ? path : path + ".meta"))));
            out.push(list, item);
        }
        out.set(root, "assets", list);
        out.save(root, index_);
    } catch (const std::exception& failure) {
        log(std::string("Could not save the asset index: ") + failure.what());
    }
}

AssetId AssetDatabase::read(const std::string& path, const Log& log) {
    const auto holder = root_ / fs::u8path(managed(path) ? path : path + ".meta");
    if (!fs::exists(holder)) return 0;
    try {
        const lton::Document document(holder);
        if (!managed(path)) document.expect(metaFormat, metaVersion);
        const auto id = lton::field(document.root(), "id");
        return lhat_is_nil(id) ? 0 : readId(id);
    } catch (const std::exception& failure) {
        log("Could not read the ID of " + path + ": " + failure.what());
        return 0;
    }
}

void AssetDatabase::write(const std::string& path, AssetId id) const {
    const auto file = root_ / fs::u8path(path);
    // A managed file is read and written again whole, with its new ID.
    if (endsWith(path, ".scene.lton")) {
        auto scene = Scene::load(file);
        scene.id = id;
        scene.save(file);
    } else if (endsWith(path, ".prefab.lton")) {
        auto prefab = Prefab::load(file);
        prefab.id = id;
        prefab.save(file);
    } else if (endsWith(path, ".level.lton")) {
        auto level = Level::load(file);
        level.id = id;
        level.save(file);
    } else if (endsWith(path, ".mixer.lton")) {
        auto mixer = MixerDefinition::load(file);
        mixer.id = id;
        mixer.save(file);
    } else {
        lton::Writer out;
        const auto root = out.table();
        out.set(root, "format", out.string(metaFormat));
        out.set(root, "version", lhat_integer(metaVersion));
        out.set(root, "id", lhat_integer(static_cast<LhatInteger>(id)));
        auto meta = file;
        meta += ".meta";
        out.save(root, meta);
    }
}

AssetId AssetDatabase::add(const std::string& path) {
    auto id = read(path, [](const std::string&) {});
    if (!id || (byId_.count(id) && byId_[id] != path)) {
        id = newAssetId();
        write(path, id);
    }
    if (const auto old = byPath_.find(path); old != byPath_.end()) byId_.erase(old->second);
    byPath_[path] = id;
    byId_[id] = path;
    return id;
}

void AssetDatabase::resave(const Log& log) const {
    const auto bind = [this, &log](AssetRef& ref) {
        const auto bound = this->bind(ref);
        if (!bound.id && !bound.path.empty() && bound.path.rfind("@ogre/", 0) != 0) log("Missing asset: " + bound.path);
        ref = bound;
    };
    for (const auto& [path, id] : byPath_) {
        const auto file = root_ / fs::u8path(path);
        try {
            if (endsWith(path, ".scene.lton")) {
                auto scene = Scene::load(file);
                forEachAssetRef(scene, bind);
                scene.save(file);
            } else if (endsWith(path, ".prefab.lton")) {
                auto prefab = Prefab::load(file);
                forEachAssetRef(prefab, bind);
                prefab.save(file);
            } else if (endsWith(path, ".level.lton")) {
                auto level = Level::load(file);
                level.forEachAssetRef(bind);
                level.save(file);
            }
        } catch (const std::exception& failure) {
            log("Could not resave " + path + ": " + failure.what());
        }
    }
}

std::string AssetDatabase::path(AssetId id) const {
    const auto found = byId_.find(id);
    return found == byId_.end() ? std::string() : found->second;
}

std::string AssetDatabase::path(const AssetRef& ref) const { return ref.id ? path(ref.id) : ref.path; }

std::string AssetDatabase::describe(const AssetRef& ref) const {
    const auto shown = path(ref);
    return shown.empty() && ref.id ? "missing asset " + std::to_string(ref.id) : shown;
}

AssetId AssetDatabase::id(const std::string& path) const {
    const auto found = byPath_.find(path);
    return found == byPath_.end() ? 0 : found->second;
}

AssetRef AssetDatabase::bind(AssetRef ref) const {
    if (ref.id || ref.path.empty()) return ref;
    if (const auto found = id(ref.path)) return AssetRef::of(found);
    return ref;
}
}
