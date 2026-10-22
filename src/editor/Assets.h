#pragma once

#include "core/AssetRef.h"
#include <lhat.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>

namespace throne {
namespace lton {
class Writer;
}

namespace lton {
/// Reads a reference: an asset's ID (an integer), or a path (a string, see relativePath());
/// nil^ is no reference.
AssetRef assetRef(LhatValue value);
/// Writes @p ref as assetRef() reads it: its ID, or else its path.
LhatValue assetRef(Writer& out, const AssetRef& ref);
/// Reads the ID a managed file or `.meta` holds of its own asset: a positive integer.
AssetId assetId(LhatValue value);
}

/// The project's assets by ID (TDDja/15 4.3): built by scanning the assets folder, where
/// managed files (levels, scenes, prefabs, mixers) carry their ID inside and every other file
/// in a `<name>.meta` beside it.
class AssetDatabase {
public:
    /// Receives what scanning found wrong or changed.
    using Log = std::function<void(const std::string&)>;

    AssetDatabase() = default;
    /// @param assets The project's assets folder.
    /// @param index Where the scan's result is cached between sessions; empty for none.
    AssetDatabase(std::filesystem::path assets, std::filesystem::path index);

    /// Reads the IDs of every file under the assets folder, giving new ones to files without
    /// (writing them into managed files or new `.meta` files) and to all but one of the files
    /// sharing one, then saves the index. Files unchanged since the cached index are not read.
    void scan(const Log& log);
    /// The assets folder.
    const std::filesystem::path& root() const { return root_; }

    /// The path of asset @p id, relative to the assets folder; empty when there is none.
    std::string path(AssetId id) const;
    /// The path @p ref names: its asset's (empty when missing), or its own.
    std::string path(const AssetRef& ref) const;
    /// The ID of the asset at @p path; 0 when there is none.
    AssetId id(const std::string& path) const;
    /// @p ref by ID when it names by path an asset of the project; otherwise unchanged.
    AssetRef bind(AssetRef ref) const;

    /// Whether files at @p path carry their ID inside (levels, scenes, prefabs, mixers).
    static bool managed(const std::string& path);
    /// Records the file the editor made at @p path, reading its ID or writing its `.meta`, and
    /// returns its ID.
    AssetId add(const std::string& path);

    /// What @p ref names, for messages: its path, or that its asset is missing.
    std::string describe(const AssetRef& ref) const;

    /// Reads every managed file and writes it again with its references by ID (paths written by
    /// hand become IDs): the migration of a project's files (TDDja/15 10). Call after scan().
    void resave(const Log& log) const;

    /// Every asset, by path.
    const std::map<std::string, AssetId>& assets() const { return byPath_; }

private:
    /// Reads, or gives and writes, the ID of the file at @p path.
    AssetId read(const std::string& path, const Log& log);
    /// Writes @p id into the file at @p path (managed) or its `.meta`.
    void write(const std::string& path, AssetId id) const;

    std::filesystem::path root_, index_;
    std::map<std::string, AssetId> byPath_;
    std::map<AssetId, std::string> byId_;
};
}
