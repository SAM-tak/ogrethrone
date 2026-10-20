#pragma once

#include "core/Math.h"
#include <array>
#include <cstdint>
#include <vector>

namespace throne {
/// A point on a surface, with the volume (of the bounds, in cubic metres) of the object it lies on.
struct SurfacePoint {
    Vec3 position;
    float volume = 0;
};

/// Adaptive Irradiance Field (TDDja/12): an octree of cells over a volume, split where surfaces are. Each
/// leaf interpolates the probes at its eight corners, which neighbouring leaves share.
/// Coordinates are whole numbers of finest cells (spacing) from lower.
struct ProbeOctree {
    /// The levels, L0 (cells spacing wide) to L5 (top cells, 32 times as wide).
    static constexpr unsigned levels = 6;
    /// The edge of a top cell, in finest cells.
    static constexpr unsigned topCells = 1u << (levels - 1);
    /// Marks a node entry as a leaf (the rest is the leaf's index), and a top cell with none.
    static constexpr std::uint32_t leafBit = 0x80000000u, none = 0xFFFFFFFFu;

    /// A leaf cell: its lowest corner and level, and the probes at its corners (x fastest).
    struct Cell {
        std::array<std::uint32_t, 3> lower{};
        unsigned level = 0;
        std::array<std::uint32_t, 8> corners{};
    };
    /// A probe: where it is, the finest level of the leaves around it, and the leaf whose
    /// corners it is interpolated from when it lies inside a coarser leaf's edge or face
    /// (a hanging point, not rendered); -1 for a probe that is rendered.
    struct Probe {
        std::array<std::uint32_t, 3> at{};
        unsigned level = 0;
        std::int32_t from = -1;
    };

    /// The box the octree was built over; its top cells may reach past upper.
    Vec3 lower, upper;
    float spacing = 1;
    /// Top cells along each axis.
    std::array<std::uint32_t, 3> top{};
    /// Per top cell (x fastest): its node entry, or none.
    std::vector<std::uint32_t> topNodes;
    /// Inner nodes' children, eight in a row (x fastest): a leaf entry or the first child's index.
    std::vector<std::uint32_t> nodes;
    std::vector<Cell> cells;
    std::vector<Probe> probes;

    /// Where @p probe is in world space.
    Vec3 position(const Probe& probe) const;
    /// The leaf holding the point @p at (in finest cells), or none.
    std::uint32_t leafAt(const Vec3& at) const;
    /// Fills in the hanging probes of @p values (one per probe, the rendered ones set) by
    /// interpolating the corners of the leaves they lie in.
    template <class Value>
    void interpolateHanging(std::vector<Value>& values) const;
};

namespace AdaptiveIrradianceField {
/// Builds the octree over the box from @p lower to @p upper, with finest cells @p spacing wide:
/// cells holding a @p surface point split, down to L0. Points on objects of smaller volume than
/// @p minObjectVolume are left out (as Unity's Min Renderer Volume Size). Top cells with no
/// point are left out unless @p fillEmptySpaces.
ProbeOctree build(const Vec3& lower, const Vec3& upper, float spacing, float minObjectVolume, bool fillEmptySpaces,
                  const std::vector<SurfacePoint>& surface);
}

template <class Value>
void ProbeOctree::interpolateHanging(std::vector<Value>& values) const {
    std::vector<char> done(probes.size());
    // Coarser leaves first: a hanging point's leaf may have hanging corners of its own, which
    // lie in a still coarser leaf.
    const auto resolve = [&](auto& self, std::size_t i) -> void {
        if (done[i] || probes[i].from < 0) return;
        const auto& cell = cells[std::size_t(probes[i].from)];
        for (const auto corner : cell.corners) self(self, corner);
        const float size = float(1u << cell.level);
        float t[3];
        for (int axis = 0; axis < 3; ++axis) t[axis] = float(probes[i].at[axis] - cell.lower[axis]) / size;
        Value sum{};
        for (unsigned c = 0; c < 8; ++c) {
            const float w = (c & 1 ? t[0] : 1 - t[0]) * (c & 2 ? t[1] : 1 - t[1]) * (c & 4 ? t[2] : 1 - t[2]);
            for (std::size_t k = 0; k < sum.size(); ++k) sum[k] += values[cell.corners[c]][k] * w;
        }
        values[i] = sum;
        done[i] = 1;
    };
    for (std::size_t i = 0; i < probes.size(); ++i) resolve(resolve, i);
}
}
