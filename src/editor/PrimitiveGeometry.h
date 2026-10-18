#pragma once
#include "editor/Scene.h"

namespace throne {
/// Built-in render geometry; a node has at most one visible primitive.
enum class PrimitiveKind { None, Box, Sphere, Capsule, Plane };
/// Geometry settings, independent of colour and transform.
struct PrimitiveShape {
    PrimitiveKind kind = PrimitiveKind::None;
    float radius = .5f, height = 2, width = 10, depth = 10;
    /// Whether the generated vertices would be identical.
    bool operator==(const PrimitiveShape& other) const {
        return kind==other.kind && radius==other.radius && height==other.height && width==other.width && depth==other.depth;
    }
    /// Negation of equality.
    bool operator!=(const PrimitiveShape& other) const { return !(*this==other); }
};
/// One vertex with a unit outward normal for physically based lighting.
struct PrimitiveVertex { Vec3 position, normal; };
/// Engine-neutral triangles and their local bounds, used for rendering and picking.
struct PrimitiveMesh {
    std::vector<PrimitiveVertex> vertices;
    std::vector<std::uint32_t> indices;
    Vec3 lower, upper;
};
/// Returns the node's render shape; throws when several render primitives are present.
PrimitiveShape primitiveShape(const SceneNode& node);
/// Returns the active primitive's colour, or white for an empty node.
std::array<float,3> primitiveColour(const SceneNode& node);
/// Builds vertices/triangles and bounds for a built-in primitive.
PrimitiveMesh primitiveMesh(const PrimitiveShape& shape);
/// Replaces the node's render primitive, preserving physics and other components.
void setPrimitive(SceneNode& node, PrimitiveKind kind);
}
