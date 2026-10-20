#include "editor/AdaptiveIrradianceField.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace throne {
namespace {
/// Packs whole-number coordinates into one key, 21 bits each.
std::uint64_t pack(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return std::uint64_t(x & 0x1fffff) | std::uint64_t(y & 0x1fffff) << 21 | std::uint64_t(z & 0x1fffff) << 42;
}

/// Builds the octree's nodes and leaves.
struct Builder {
    ProbeOctree& out;
    const std::vector<Vec3>& points;
    std::unordered_map<std::uint64_t, std::uint32_t> probeAt;

    /// The entry for the cell at @p lower (finest cells) of @p level holding @p held points.
    std::uint32_t cell(const std::array<std::uint32_t, 3>& lower, unsigned level, const std::vector<std::uint32_t>& held) {
        if (level == 0 || held.empty()) return leaf(lower, level);
        // The children take eight nodes in a row, reserved before they are filled.
        const auto first = std::uint32_t(out.nodes.size());
        out.nodes.resize(out.nodes.size() + 8);
        const std::uint32_t half = 1u << (level - 1);
        std::array<std::vector<std::uint32_t>, 8> inChild;
        for (const auto i : held) {
            const auto& p = points[i];
            const unsigned c = unsigned(p.x >= float(lower[0] + half)) | unsigned(p.y >= float(lower[1] + half)) << 1 |
                               unsigned(p.z >= float(lower[2] + half)) << 2;
            inChild[c].push_back(i);
        }
        for (unsigned c = 0; c < 8; ++c) {
            const std::array<std::uint32_t, 3> at{lower[0] + (c & 1 ? half : 0), lower[1] + (c & 2 ? half : 0),
                                                  lower[2] + (c & 4 ? half : 0)};
            const auto entry = cell(at, level - 1, inChild[c]);
            out.nodes[first + c] = entry;
        }
        return first;
    }

    /// Adds a leaf and the probes at its corners.
    std::uint32_t leaf(const std::array<std::uint32_t, 3>& lower, unsigned level) {
        ProbeOctree::Cell c;
        c.lower = lower;
        c.level = level;
        const std::uint32_t size = 1u << level;
        for (unsigned k = 0; k < 8; ++k) {
            const std::array<std::uint32_t, 3> at{lower[0] + (k & 1 ? size : 0), lower[1] + (k & 2 ? size : 0),
                                                  lower[2] + (k & 4 ? size : 0)};
            const auto [it, added] = probeAt.try_emplace(pack(at[0], at[1], at[2]), std::uint32_t(out.probes.size()));
            if (added) out.probes.push_back({at, level, -1});
            auto& probe = out.probes[it->second];
            probe.level = std::min(probe.level, level);
            c.corners[k] = it->second;
        }
        out.cells.push_back(c);
        return ProbeOctree::leafBit | std::uint32_t(out.cells.size() - 1);
    }
};
}

Vec3 ProbeOctree::position(const Probe& probe) const {
    return lower + Vec3{float(probe.at[0]), float(probe.at[1]), float(probe.at[2])} * spacing;
}

std::uint32_t ProbeOctree::leafAt(const Vec3& at) const {
    if (at.x < 0 || at.y < 0 || at.z < 0) return none;
    const auto tx = std::uint32_t(at.x) / topCells, ty = std::uint32_t(at.y) / topCells, tz = std::uint32_t(at.z) / topCells;
    if (tx >= top[0] || ty >= top[1] || tz >= top[2]) return none;
    auto entry = topNodes[(std::size_t(tz) * top[1] + ty) * top[0] + tx];
    if (entry == none) return none;
    std::array<float, 3> corner{float(tx * topCells), float(ty * topCells), float(tz * topCells)};
    for (float size = float(topCells); !(entry & leafBit); size *= .5f) {
        const float half = size * .5f;
        const unsigned c = unsigned(at.x >= corner[0] + half) | unsigned(at.y >= corner[1] + half) << 1 |
                           unsigned(at.z >= corner[2] + half) << 2;
        for (unsigned axis = 0; axis < 3; ++axis)
            if (c >> axis & 1) corner[axis] += half;
        entry = nodes[entry + c];
    }
    return entry & ~leafBit;
}

ProbeOctree AdaptiveIrradianceField::build(const Vec3& lower, const Vec3& upper, float spacing, float minObjectVolume,
                                  bool fillEmptySpaces, const std::vector<SurfacePoint>& surface) {
    ProbeOctree out;
    out.lower = lower;
    out.upper = upper;
    out.spacing = spacing;
    if (!(spacing > 0)) return out;
    const float topSize = spacing * float(ProbeOctree::topCells);
    for (unsigned axis = 0; axis < 3; ++axis)
        out.top[axis] = std::max(1u, std::uint32_t(std::ceil(((&upper.x)[axis] - (&lower.x)[axis]) / topSize)));
    // The points in finest cells from lower, gathered by top cell.
    std::vector<Vec3> points;
    std::vector<std::vector<std::uint32_t>> held(std::size_t(out.top[0]) * out.top[1] * out.top[2]);
    for (const auto& point : surface) {
        if (point.volume < minObjectVolume) continue;
        const auto p = (point.position - lower) * (1 / spacing);
        if (p.x < 0 || p.y < 0 || p.z < 0) continue;
        const auto tx = std::uint32_t(p.x) / ProbeOctree::topCells, ty = std::uint32_t(p.y) / ProbeOctree::topCells,
                   tz = std::uint32_t(p.z) / ProbeOctree::topCells;
        if (tx >= out.top[0] || ty >= out.top[1] || tz >= out.top[2]) continue;
        held[(std::size_t(tz) * out.top[1] + ty) * out.top[0] + tx].push_back(std::uint32_t(points.size()));
        points.push_back(p);
    }
    Builder builder{out, points, {}};
    out.topNodes.assign(held.size(), ProbeOctree::none);
    for (std::uint32_t z = 0; z < out.top[2]; ++z)
        for (std::uint32_t y = 0; y < out.top[1]; ++y)
            for (std::uint32_t x = 0; x < out.top[0]; ++x) {
                const auto i = (std::size_t(z) * out.top[1] + y) * out.top[0] + x;
                if (held[i].empty() && !fillEmptySpaces) continue;
                const std::array<std::uint32_t, 3> at{x * ProbeOctree::topCells, y * ProbeOctree::topCells,
                                                      z * ProbeOctree::topCells};
                out.topNodes[i] = builder.cell(at, ProbeOctree::levels - 1, held[i]);
            }
    // A probe inside an edge or face of a coarser leaf (not one of its corners) hangs from it;
    // the leaves around a probe are those half a finest cell away from it on each side.
    for (auto& probe : out.probes) {
        unsigned coarsest = 0;
        for (unsigned k = 0; k < 8; ++k) {
            const Vec3 near{float(probe.at[0]) + (k & 1 ? .5f : -.5f), float(probe.at[1]) + (k & 2 ? .5f : -.5f),
                            float(probe.at[2]) + (k & 4 ? .5f : -.5f)};
            const auto leaf = out.leafAt(near);
            if (leaf == ProbeOctree::none) continue;
            const auto& cell = out.cells[leaf];
            const std::uint32_t size = 1u << cell.level;
            bool corner = true;
            for (unsigned axis = 0; axis < 3; ++axis) {
                const auto offset = probe.at[axis] - cell.lower[axis];
                corner = corner && (offset == 0 || offset == size);
            }
            if (!corner && cell.level >= coarsest) {
                coarsest = cell.level;
                probe.from = std::int32_t(leaf);
            }
        }
    }
    return out;
}
}
