#pragma once
#include "core/AssetRef.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace throne {
struct MixerBus {
    std::uint32_t id = 1, parent = 0;
    std::string name = "Master";
    float volume = 1;
    bool mute = false;
};
struct MixerDefinition {
    /// The file's own asset ID (TDDja/15 2).
    AssetId id = newAssetId();
    std::vector<MixerBus> buses{{}};
    /// High-water mark; deleting a bus never reuses an authored ID.
    std::uint64_t nextBusId = 2;
    static MixerDefinition defaults();
    void validate() const;
    void save(const std::filesystem::path& path) const;
    static MixerDefinition load(const std::filesystem::path& path);
};
}
