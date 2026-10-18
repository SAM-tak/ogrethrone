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
    /// The view instance the runner runs in, or -1 for a runner of the level itself.
    int instance;
    /// The prefab instance inside it the runner came from; empty for the file's own runner.
    NodePath from;
    /// Zero for an attachment on the node at `from`; otherwise the direct runner ID.
    std::uint32_t runner;
    std::size_t behaviour;
    std::string field;
    /// "runner > script > field", for display.
    std::string label;
};

/// The editor's reverse index of node references: every behaviour field referring
/// to a node, keyed by (view instance, index in its expansion), with field
/// overrides applied, as Play would resolve them. Built from a view, kept by the
/// editor alone, and so not part of the data or of a game build.
using References = std::map<std::pair<std::size_t, std::size_t>, std::vector<Referrer>>;

/// Resolves the node references of @p view's runners, and of @p level's own
/// runners when it is given (at the Level step).
References findReferences(const LevelExpansion& view, const Level* level);
}
