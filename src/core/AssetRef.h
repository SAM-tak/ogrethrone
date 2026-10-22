#pragma once

#include <cstdint>
#include <string>
#include <tuple>

namespace throne {
/// The persistent ID of an asset (TDDja/15 4.1): a positive 64-bit Snowflake ID that moving,
/// renaming or editing the file keeps. 0 is none.
using AssetId = std::uint64_t;
/// Returns a new asset ID from the assets' own generator. Not thread safe.
AssetId newAssetId();

/// A reference to a file (TDDja/15 5): an asset of the project by its ID, or by a path where
/// there is none: a file outside the assets (OGRE's sample media, `@ogre/...`), or a path
/// written by hand that has not been bound to an asset (AssetDatabase::bind()).
struct AssetRef {
    AssetId id = 0;
    /// The path, relative to the assets folder, of a reference without an ID.
    std::string path;

    AssetRef() = default;
    /// A reference by path; implicit, so paths can be written where references go.
    AssetRef(std::string path) : path(std::move(path)) {}
    AssetRef(const char* path) : path(path) {}
    /// A reference to the asset @p id.
    static AssetRef of(AssetId id) {
        AssetRef ref;
        ref.id = id;
        return ref;
    }
    /// Whether it refers to nothing.
    bool empty() const { return !id && path.empty(); }
    /// Makes it refer to nothing.
    void clear() { *this = {}; }
    bool operator==(const AssetRef& other) const { return id == other.id && path == other.path; }
    bool operator!=(const AssetRef& other) const { return !(*this == other); }
    /// An order for maps keyed by references.
    bool operator<(const AssetRef& other) const { return std::tie(id, path) < std::tie(other.id, other.path); }
};

}
