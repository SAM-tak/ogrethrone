#pragma once

#include "editor/Expansion.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace throne {
/// One behaviour field that refers to a node.
struct Referrer {
    /// The view instance the system runs in, or -1 for a system of the level itself.
    int instance;
    /// The prefab instance inside it the system came from; empty for the file's own system.
    NodePath from;
    /// Zero for an attachment on the node at `from`; otherwise the direct system ID.
    std::uint32_t system;
    std::size_t behaviour;
    std::string field;
    /// "system > script > field", for display.
    std::string label;
};

/// The editor's reverse index of node references: every behaviour field referring
/// to a node, keyed by (view instance, index in its expansion), with field
/// overrides applied, as Play would resolve them. Built from a view, kept by the
/// editor alone, and so not part of the data or of a game build.
using References = std::map<std::pair<std::size_t, std::size_t>, std::vector<Referrer>>;

/// Resolves the node references of @p view's systems, and of @p level's own
/// systems when it is given (at the Level step).
References findReferences(const LevelExpansion& view, const Level* level);
}
