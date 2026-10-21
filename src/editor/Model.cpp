#include "editor/Model.h"
#include <OgreHlmsPbs.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreItem.h>
#include <OgreSubItem.h>
#include <OgreMeshManager2.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh2.h>
#include <OgreTextureGpuManager.h>
#include <OgreRenderSystem.h>
#include <Vao/OgreVaoManager.h>
#include <ufbx.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <unordered_map>

namespace throne {
namespace {
/// Throne reads ORM maps as glTF packs them: occlusion, roughness, metalness in R, G, B.
/// HlmsPbs samples both its roughness and its metalness from R, so materials that
/// use one ORM map for both re-point the two samplers at G and B.
constexpr const char* ormPiece = R"(@piece( custom_ps_posMaterialLoad )
	#undef SampleSpecular
	#define SampleSpecular( tex, sampler, uv, arrayIdx ) OGRE_SampleArray2DF16( tex, sampler, uv, arrayIdx ).zzzz
	#undef SampleRoughness
	#define SampleRoughness( tex, sampler, uv, arrayIdx ) OGRE_SampleArray2DF16( tex, sampler, uv, arrayIdx ).yyyy
@end
)";

/// The pixel-shader piece that outputs a volume's Beer-Lambert transmittance over a path
/// of @p thickness times the cosine of the view angle (the chord through a rounded volume
/// is longest through its middle); a multiplying blend then tints what is behind it. The
/// cosine ignores which way the file's normal points, as some liquids face inwards.
std::string absorptionPiece(const Vec3& absorption, float thickness) {
    char text[384];
    std::snprintf(text, sizeof text,
                  "@piece( custom_ps_posExecution )\n@property( !hlms_shadowcaster )\n"
                  "\toutPs_colour0 = midf4_c( exp( -float3( %g, %g, %g ) * ( %g * abs( float( dot( pixelData.normal, pixelData.viewDir ) ) ) ) ), 1.0 );\n"
                  "@end\n@end\n",
                  absorption.x, absorption.y, absorption.z, thickness);
    return text;
}

} // namespace

std::string surfacePiece(const Translucency& t, float subsurfaceWidth) {
    std::string pieces;
    char text[1024];
    if (subsurfaceWidth > 0) {
        std::snprintf(text, sizeof text, "#define THRONE_SUBSURFACE_WIDTH %f\n", subsurfaceWidth);
        pieces += text;
    }
    if (!t.enabled())
        return pieces.empty() ? pieces : "@piece( custom_ps_uniformStructDeclaration )\n" + pieces + "@end\n";
    // The rename comes before DeclareBRDF; the wrapper, under BRDF's own conditions, after it.
    constexpr const char* format = R"(@piece( custom_ps_uniformStructDeclaration )
%s#define BRDF throneSurfaceBRDF
@end
@piece( custom_ps_functions )
@property( (hlms_normal || hlms_qtangent) && !hlms_prepass && needs_view_dir )
#undef BRDF
INLINE midf3 BRDF( midf3 lightDir, midf3 lightDiffuse, midf3 lightSpecular, PixelData pixelData PASSBUF_ARG_DECL )
{
	midf3 through = -( lightDir + pixelData.normal * midf_c( %f ) );
	midf transmitted = pow( saturate( dot( pixelData.viewDir, through ) ), midf_c( %f ) ) * midf_c( %f );
	return throneSurfaceBRDF( lightDir, lightDiffuse, lightSpecular, pixelData PASSBUF_ARG ) +
		lightDiffuse * midf3_c( %f, %f, %f ) * transmitted;
}
@end
@end
)";
    std::snprintf(text, sizeof text, format, pieces.c_str(), t.distortion, t.power, t.scale, t.colour.x, t.colour.y, t.colour.z);
    return text;
}

namespace {
/// One vertex as the file gives it, before tangents are derived.
struct Corner {
    float position[3], normal[3], uv[2];
};
/// One vertex as OGRE draws it.
struct Vertex {
    float position[3], normal[3], tangent[4], uv[2];
};

Vec3 vec(const ufbx_vec3& v) { return {float(v.x), float(v.y), float(v.z)}; }
Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalized(const Vec3& v) {
    const float length = std::sqrt(dot(v, v));
    return length > 1e-20f ? v * (1 / length) : Vec3{0, 0, 0};
}

/// Whether the DDS file at @p file stores an alpha channel (DXT5 / BC3), read from its header.
bool ddsHasAlpha(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    char header[88] = {};
    if (!in.read(header, sizeof header) || std::memcmp(header, "DDS ", 4) != 0) return false;
    return std::memcmp(header + 84, "DXT5", 4) == 0 || std::memcmp(header + 84, "DXT4", 4) == 0;
}

/// Computes per-vertex tangents from the triangles; the sign (w) gives the bitangent's
/// side, flipped for normal maps whose green points up (OpenGL's convention).
void deriveTangents(const std::vector<Corner>& corners, const std::vector<uint32_t>& indices, bool directX,
                    std::vector<Vertex>& out) {
    std::vector<Vec3> tangents(corners.size()), bitangents(corners.size());
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto& a = corners[indices[i]];
        const auto& b = corners[indices[i + 1]];
        const auto& c = corners[indices[i + 2]];
        const Vec3 e1{b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
        const Vec3 e2{c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
        const float du1 = b.uv[0] - a.uv[0], dv1 = b.uv[1] - a.uv[1], du2 = c.uv[0] - a.uv[0], dv2 = c.uv[1] - a.uv[1];
        const float r = du1 * dv2 - du2 * dv1;
        if (std::fabs(r) < 1e-20f) continue;
        const float f = 1 / r;
        const auto t = (e1 * dv2 - e2 * dv1) * f;
        const auto s = (e2 * du1 - e1 * du2) * f;
        for (int k = 0; k < 3; ++k) {
            tangents[indices[i + k]] = tangents[indices[i + k]] + t;
            bitangents[indices[i + k]] = bitangents[indices[i + k]] + s;
        }
    }
    out.resize(corners.size());
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const auto& corner = corners[i];
        const Vec3 n = normalized({corner.normal[0], corner.normal[1], corner.normal[2]});
        auto t = normalized(tangents[i] - n * dot(n, tangents[i]));
        // A vertex no triangle gave a direction to still needs a tangent at right angles.
        if (dot(t, t) == 0) t = normalized(std::fabs(n.x) < 0.9f ? cross(n, {1, 0, 0}) : cross(n, {0, 1, 0}));
        float w = dot(cross(n, t), bitangents[i]) < 0 ? -1.0f : 1.0f;
        if (!directX) w = -w;
        auto& vertex = out[i];
        std::memcpy(vertex.position, corner.position, sizeof vertex.position);
        vertex.normal[0] = n.x, vertex.normal[1] = n.y, vertex.normal[2] = n.z;
        vertex.tangent[0] = t.x, vertex.tangent[1] = t.y, vertex.tangent[2] = t.z, vertex.tangent[3] = w;
        std::memcpy(vertex.uv, corner.uv, sizeof vertex.uv);
    }
}

/// The connected pieces of a mesh: faces sharing a vertex position (welded where the file
/// repeats a position) are in one piece. A file often merges many objects into one mesh;
/// its pieces stand for them.
struct Pieces {
    /// The piece of each face.
    std::vector<std::uint32_t> ofFace;
    /// Each piece's bounds, in the mesh's space.
    std::vector<Vec3> lower, upper;
};

/// Finds the pieces of @p mesh.
Pieces findPieces(const ufbx_mesh& mesh) {
    // Union-find over welded positions, keyed by a hash of their bits (collisions, which would
    // only join two pieces, are negligible).
    std::unordered_map<std::uint64_t, std::uint32_t> welded;
    std::vector<std::uint32_t> position(mesh.vertex_position.values.count), parent;
    for (std::size_t i = 0; i < position.size(); ++i) {
        const auto v = vec(mesh.vertex_position.values.data[i]);
        std::uint32_t bits[3];
        std::memcpy(bits, &v, sizeof bits);
        const auto key = std::uint64_t(bits[0]) * 0x9e3779b97f4a7c15ull ^ std::uint64_t(bits[1]) * 0xc2b2ae3d27d4eb4full ^
                         std::uint64_t(bits[2]) * 0x165667b19e3779f9ull;
        const auto [at, added] = welded.try_emplace(key, std::uint32_t(parent.size()));
        if (added) parent.push_back(std::uint32_t(parent.size()));
        position[i] = at->second;
    }
    const auto root = [&](std::uint32_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    const auto vertex = [&](const ufbx_face& face, std::uint32_t corner) {
        return position[mesh.vertex_position.indices.data[face.index_begin + corner]];
    };
    for (const auto& face : mesh.faces)
        for (std::uint32_t c = 1; c < face.num_indices; ++c) parent[root(vertex(face, c))] = root(vertex(face, 0));
    Pieces out;
    std::unordered_map<std::uint32_t, std::uint32_t> pieceOf;
    out.ofFace.resize(mesh.faces.count);
    for (std::size_t f = 0; f < mesh.faces.count; ++f) {
        const auto& face = mesh.faces.data[f];
        if (face.num_indices == 0) continue;
        const auto [at, added] = pieceOf.try_emplace(root(vertex(face, 0)), std::uint32_t(out.lower.size()));
        if (added) {
            out.lower.push_back({std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()});
            out.upper.push_back(out.lower.back() * -1.0f);
        }
        out.ofFace[f] = at->second;
        auto &lower = out.lower[at->second], &upper = out.upper[at->second];
        for (std::uint32_t c = 0; c < face.num_indices; ++c) {
            const auto p = vec(ufbx_get_vertex_vec3(&mesh.vertex_position, face.index_begin + c));
            lower = {std::min(lower.x, p.x), std::min(lower.y, p.y), std::min(lower.z, p.z)};
            upper = {std::max(upper.x, p.x), std::max(upper.y, p.y), std::max(upper.z, p.z)};
        }
    }
    return out;
}

/// The volume of the bounds of the box from @p lower to @p upper placed by @p transform, each
/// extent at least 1 cm: a flat piece (a ground, a wall) would otherwise have no volume.
float placedVolume(const ufbx_matrix& transform, const Vec3& lower, const Vec3& upper) {
    Vec3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 high = low * -1.0f;
    for (int corner = 0; corner < 8; ++corner) {
        const auto p = vec(ufbx_transform_position(&transform, {corner & 1 ? upper.x : lower.x, corner & 2 ? upper.y : lower.y,
                                                                corner & 4 ? upper.z : lower.z}));
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    const auto extent = [](float from, float to) { return std::max(to - from, .01f); };
    return extent(low.x, high.x) * extent(low.y, high.y) * extent(low.z, high.z);
}
}

ModelLibrary::ModelLibrary(Ogre::Root& root, Ogre::SceneManager& scene, Ogre::HlmsPbs& pbs, Log log)
    : root_(root), scene_(scene), pbs_(pbs), log_(std::move(log)) {}

ModelLibrary::~ModelLibrary() {
    for (auto& [key, model] : models_) {
        for (auto& mesh : model.meshes) Ogre::MeshManager::getSingleton().remove(mesh);
        for (auto* material : model.materials) pbs_.destroyDatablock(material->getName());
    }
}

std::string ModelLibrary::key(const ModelComponent& component, const std::filesystem::path& file) {
    auto key = file.generic_u8string() + (component.directXNormals ? "#dx" : "#gl") + "#" + std::to_string(component.emission) +
               (component.alphaHashing ? "#hash" : "");
    for (const auto& m : component.materials)
        key += "#" + m.name + "=" + std::to_string(m.absorption.x) + "," + std::to_string(m.absorption.y) + "," +
               std::to_string(m.absorption.z) + "/" + std::to_string(m.thickness) + "/" + std::to_string(m.translucency.colour.x) +
               "," + std::to_string(m.translucency.colour.y) + "," + std::to_string(m.translucency.colour.z) + "," +
               std::to_string(m.translucency.distortion) + "," + std::to_string(m.translucency.power) + "," +
               std::to_string(m.translucency.scale) + "/" + std::to_string(m.subsurfaceWidth);
    return key;
}

ModelLibrary::Placed ModelLibrary::place(const std::string& key, Ogre::SceneNode& parent) {
    const auto found = models_.find(key);
    if (found == models_.end() || found->second.parts.empty()) return {};
    const auto& model = found->second;
    Placed placed{parent.createChildSceneNode(), {}, model.lower, model.upper};
    for (const auto& part : model.parts) {
        auto* node = placed.root->createChildSceneNode();
        node->setPosition(part.position.x, part.position.y, part.position.z);
        node->setOrientation(Ogre::Quaternion(part.rotation[3], part.rotation[0], part.rotation[1], part.rotation[2]));
        node->setScale(part.scale.x, part.scale.y, part.scale.z);
        auto* item = scene_.createItem(part.mesh);
        item->setCastShadows(part.renderQueue == 0);
        if (part.renderQueue) item->setRenderQueueGroup(part.renderQueue);
        node->attachObject(item);
        // Hidden after attaching, which shows it.
        item->setVisible(false);
        placed.items.push_back(item);
        if (hidden_.empty()) hiddenTotal_ = 0;
        hidden_.push_back(item);
        ++hiddenTotal_;
    }
    return placed;
}

void ModelLibrary::reveal(unsigned materials) {
    std::size_t kept = 0;
    // The first item with new materials is shown even beyond the budget, or one using more
    // new materials than it would never be.
    bool spent = false;
    for (auto* item : hidden_) {
        std::vector<const void*> fresh;
        for (std::size_t i = 0; i < item->getNumSubItems(); ++i) {
            const void* material = item->getSubItem(i)->getDatablock();
            if (!shownMaterials_.count(material) && std::find(fresh.begin(), fresh.end(), material) == fresh.end())
                fresh.push_back(material);
        }
        if (!fresh.empty() && fresh.size() > materials && spent) {
            hidden_[kept++] = item;
            continue;
        }
        if (!fresh.empty()) spent = true;
        materials -= (std::min)(materials, unsigned(fresh.size()));
        shownMaterials_.insert(fresh.begin(), fresh.end());
        item->setVisible(true);
    }
    hidden_.resize(kept);
}

const std::vector<SurfacePoint>& ModelLibrary::surface(const std::string& key) const {
    static const std::vector<SurfacePoint> none;
    const auto found = models_.find(key);
    return found == models_.end() ? none : found->second.surface;
}

void ModelLibrary::remove(Placed& placed) {
    for (auto* item : placed.items) {
        hidden_.erase(std::remove(hidden_.begin(), hidden_.end(), item), hidden_.end());
        scene_.destroyItem(item);
    }
    if (placed.root) {
        placed.root->removeAndDestroyAllChildren();
        scene_.destroySceneNode(placed.root);
    }
    placed = {};
}

void ModelLibrary::setReflection(Ogre::TextureGpu* cubemap) {
    reflection_ = cubemap;
    for (auto& [key, model] : models_)
        for (auto* material : model.materials) material->setTexture(Ogre::PBSM_REFLECTION, cubemap);
}

ModelLibrary::Parsed ModelLibrary::parse(const std::filesystem::path& file, const ModelComponent& component) {
    Parsed out;
    ufbx_load_opts options = {};
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0f;
    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(file.u8string().c_str(), &options, &error);
    if (!scene) {
        out.messages.push_back("Cannot read model " + file.u8string() + ": " + error.description.data);
        return out;
    }
    const auto folder = file.parent_path();
    // Texture files are named relative to the model, or found in its Textures folder.
    const auto texture = [&](const ufbx_texture* source) -> std::filesystem::path {
        if (!source) return {};
        std::string relative(source->relative_filename.data, source->relative_filename.length);
        std::replace(relative.begin(), relative.end(), '\\', '/');
        auto path = folder / std::filesystem::u8path(relative);
        if (!std::filesystem::exists(path)) path = folder / "Textures" / std::filesystem::u8path(relative).filename();
        if (std::filesystem::exists(path)) return path;
        out.messages.push_back("Model texture not found: " + path.u8string());
        return {};
    };
    for (std::size_t i = 0; i < scene->materials.count; ++i) {
        const auto* source = scene->materials.data[i];
        Parsed::Material material;
        material.name = source->name.data;
        material.diffuse = texture(source->fbx.diffuse_color.texture);
        // The file's transparency colour makes glass and liquids blended; they keep their reflections.
        const auto& clear = source->fbx.transparency_color.value_vec3;
        material.opacity = 1.0f - float(source->fbx.transparency_factor.value_real * (clear.x + clear.y + clear.z) / 3);
        material.blended = material.opacity < .99f;
        material.alphaTested = !material.blended && !material.diffuse.empty() && ddsHasAlpha(material.diffuse);
        material.alphaHashed = material.alphaTested && component.alphaHashing;
        material.colour = vec(source->fbx.diffuse_color.value_vec3);
        material.normal = texture(source->fbx.normal_map.texture);
        // Glass and liquids are smooth dielectrics; their files' ORM maps make them look frosted.
        if (!material.blended) material.orm = texture(source->fbx.specular_color.texture);
        material.emissive = texture(source->fbx.emission_color.texture);
        material.emission = component.emission;
        // A volume with absorption (a liquid, tinted glass) multiplies what is drawn behind it
        // by its transmittance, last in the opaque pass. Other blended surfaces (clear glass)
        // refract that pass afterwards, so a glass shows the wine in it.
        const auto change = std::find_if(component.materials.begin(), component.materials.end(),
                                         [&](const ModelMaterial& m) { return m.name == material.name; });
        const bool absorbs = material.blended && change != component.materials.end() &&
                             (change->absorption.x > 0 || change->absorption.y > 0 || change->absorption.z > 0);
        if (absorbs) material.pieces += absorptionPiece(change->absorption, change->thickness);
        if (!material.orm.empty()) material.pieces += ormPiece;
        if (!material.blended && change != component.materials.end())
            material.pieces += surfacePiece(change->translucency, change->subsurfaceWidth);
        material.renderQueue = absorbs ? 99 : material.blended ? 200 : 0;
        out.materials.push_back(std::move(material));
    }

    // One mesh per file mesh, one submesh per material it uses.
    std::map<const ufbx_mesh*, std::size_t> meshes;
    std::vector<uint32_t> triangle;
    for (std::size_t m = 0; m < scene->meshes.count; ++m) {
        const auto* source = scene->meshes.data[m];
        if (!source->vertex_position.exists || source->num_triangles == 0) continue;
        Parsed::Mesh mesh;
        mesh.lower = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        mesh.upper = mesh.lower * -1.0f;
        triangle.resize(source->max_face_triangles * 3);
        for (const auto& part : source->material_parts) {
            if (part.num_triangles == 0) continue;
            std::vector<Corner> corners;
            corners.reserve(part.num_triangles * 3);
            for (const auto face : part.face_indices) {
                const auto count = ufbx_triangulate_face(triangle.data(), triangle.size(), source, source->faces.data[face]);
                for (std::size_t c = 0; c < count * 3; ++c) {
                    const auto index = triangle[c];
                    Corner corner{};
                    const auto p = ufbx_get_vertex_vec3(&source->vertex_position, index);
                    corner.position[0] = float(p.x), corner.position[1] = float(p.y), corner.position[2] = float(p.z);
                    if (source->vertex_normal.exists) {
                        const auto n = ufbx_get_vertex_vec3(&source->vertex_normal, index);
                        corner.normal[0] = float(n.x), corner.normal[1] = float(n.y), corner.normal[2] = float(n.z);
                    }
                    if (source->vertex_uv.exists) {
                        const auto uv = ufbx_get_vertex_vec2(&source->vertex_uv, index);
                        // FBX puts the origin bottom-left; OGRE samples from the top left.
                        corner.uv[0] = float(uv.x), corner.uv[1] = 1.0f - float(uv.y);
                    }
                    corners.push_back(corner);
                }
            }
            // Shares identical corners between triangles.
            Parsed::Submesh submesh;
            submesh.indices.resize(corners.size());
            ufbx_vertex_stream stream{corners.data(), corners.size(), sizeof(Corner)};
            const auto unique = ufbx_generate_indices(&stream, 1, submesh.indices.data(), submesh.indices.size(), nullptr, &error);
            if (error.type != UFBX_ERROR_NONE) continue;
            corners.resize(unique);
            std::vector<Vertex> vertices;
            deriveTangents(corners, submesh.indices, component.directXNormals, vertices);
            for (const auto& v : vertices) {
                mesh.lower = {std::min(mesh.lower.x, v.position[0]), std::min(mesh.lower.y, v.position[1]), std::min(mesh.lower.z, v.position[2])};
                mesh.upper = {std::max(mesh.upper.x, v.position[0]), std::max(mesh.upper.y, v.position[1]), std::max(mesh.upper.z, v.position[2])};
            }
            submesh.vertices.resize(vertices.size() * (sizeof(Vertex) / sizeof(float)));
            std::memcpy(submesh.vertices.data(), vertices.data(), vertices.size() * sizeof(Vertex));
            const auto* material = part.index < source->materials.count ? source->materials.data[part.index] : nullptr;
            submesh.material = material ? material->typed_id : out.materials.size();
            mesh.submeshes.push_back(std::move(submesh));
        }
        if (mesh.submeshes.empty()) continue;
        meshes[source] = out.meshes.size();
        out.meshes.push_back(std::move(mesh));
    }

    // Every node with a mesh places it; the bounds gather the placed boxes' corners.
    out.lower = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    out.upper = out.lower * -1.0f;
    // Surface cells, packed as 21-bit signed coordinates, with the largest piece (by volume) touching each.
    std::unordered_map<std::uint64_t, float> cells;
    std::map<const ufbx_mesh*, Pieces> pieces;
    std::vector<float> pieceVolume;
    const auto touch = [&](const Vec3& p, float size) {
        const auto c = [](float v) { return std::uint64_t(std::int64_t(std::floor(v / surfaceCell)) + (1 << 20)) & 0x1fffff; };
        auto& largest = cells[c(p.x) | c(p.y) << 21 | c(p.z) << 42];
        largest = std::max(largest, size);
    };
    for (std::size_t n = 0; n < scene->nodes.count; ++n) {
        const auto* node = scene->nodes.data[n];
        const auto found = node->mesh ? meshes.find(node->mesh) : meshes.end();
        if (found == meshes.end()) continue;
        const auto transform = ufbx_matrix_to_transform(&node->geometry_to_world);
        Parsed::Part part;
        part.mesh = found->second;
        part.position = vec(transform.translation);
        part.scale = vec(transform.scale);
        part.rotation[0] = float(transform.rotation.x), part.rotation[1] = float(transform.rotation.y);
        part.rotation[2] = float(transform.rotation.z), part.rotation[3] = float(transform.rotation.w);
        const auto& mesh = out.meshes[found->second];
        Vec3 partLower{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        Vec3 partUpper = partLower * -1.0f;
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 local{corner & 1 ? mesh.upper.x : mesh.lower.x, corner & 2 ? mesh.upper.y : mesh.lower.y,
                             corner & 4 ? mesh.upper.z : mesh.lower.z};
            const auto world = vec(ufbx_transform_position(&node->geometry_to_world, {local.x, local.y, local.z}));
            partLower = {std::min(partLower.x, world.x), std::min(partLower.y, world.y), std::min(partLower.z, world.z)};
            partUpper = {std::max(partUpper.x, world.x), std::max(partUpper.y, world.y), std::max(partUpper.z, world.z)};
        }
        out.lower = {std::min(out.lower.x, partLower.x), std::min(out.lower.y, partLower.y), std::min(out.lower.z, partLower.z)};
        out.upper = {std::max(out.upper.x, partUpper.x), std::max(out.upper.y, partUpper.y), std::max(out.upper.z, partUpper.z)};
        // Each triangle is sampled on a barycentric grid fine enough to touch every cell it
        // crosses, with the volume of its piece as placed.
        const auto& source = *node->mesh;
        triangle.resize(source.max_face_triangles * 3);
        const auto [at, added] = pieces.try_emplace(&source);
        if (added) at->second = findPieces(source);
        const auto& meshPieces = at->second;
        pieceVolume.resize(meshPieces.lower.size());
        for (std::size_t i = 0; i < pieceVolume.size(); ++i)
            pieceVolume[i] = placedVolume(node->geometry_to_world, meshPieces.lower[i], meshPieces.upper[i]);
        for (std::size_t f = 0; f < source.faces.count; ++f) {
            const auto count = ufbx_triangulate_face(triangle.data(), triangle.size(), &source, source.faces.data[f]);
            for (std::size_t t = 0; t < count; ++t) {
                Vec3 corners[3];
                for (int c = 0; c < 3; ++c)
                    corners[c] = vec(ufbx_transform_position(&node->geometry_to_world,
                                                             ufbx_get_vertex_vec3(&source.vertex_position, triangle[t * 3 + c])));
                const auto &a = corners[0], &b = corners[1], &c = corners[2];
                const auto length = [](const Vec3& v) { return std::sqrt(dot(v, v)); };
                const float edge = std::max({length(b - a), length(c - b), length(a - c)});
                const int steps = std::max(1, int(std::ceil(edge / surfaceCell)));
                for (int u = 0; u <= steps; ++u)
                    for (int w = 0; u + w <= steps; ++w)
                        touch(a + (b - a) * (float(u) / steps) + (c - a) * (float(w) / steps), pieceVolume[meshPieces.ofFace[f]]);
            }
        }
        out.parts.push_back(part);
    }
    out.surface.reserve(cells.size());
    for (const auto& [cell, volume] : cells) {
        const auto c = [key = cell](int shift) { return (float(std::int64_t(key >> shift & 0x1fffff) - (1 << 20)) + .5f) * surfaceCell; };
        out.surface.push_back({{c(0), c(21), c(42)}, volume});
    }
    ufbx_free_scene(scene);
    if (out.parts.empty()) {
        out.messages.push_back("Model " + file.u8string() + " has no meshes");
        out.lower = out.upper = {};
    }
    return out;
}

void ModelLibrary::add(const std::string& key, const Parsed& parsed) {
    for (const auto& message : parsed.messages) log_(message);
    Model out;
    out.lower = parsed.lower, out.upper = parsed.upper;
    out.surface = parsed.surface;
    // Names stay unique when the same file is read with other material changes.
    const auto prefix = "ThroneModel/" + std::to_string(models_.size()) + "/";
    auto& resources = Ogre::ResourceGroupManager::getSingleton();
    auto* textures = root_.getRenderSystem()->getTextureGpuManager();
    // Textures are found through a resource group per folder, named after the folder.
    const auto texture = [&](const std::filesystem::path& path, Ogre::CommonTextureTypes::CommonTextureTypes kind) -> Ogre::TextureGpu* {
        if (path.empty()) return nullptr;
        const auto location = path.parent_path().generic_u8string();
        const Ogre::String group = "ThroneModels/" + location;
        if (!resources.resourceGroupExists(group)) resources.createResourceGroup(group);
        if (!resources.resourceLocationExists(location, group)) resources.addResourceLocation(location, "FileSystem", group);
        return textures->createOrRetrieveTexture(path.filename().u8string(), path.generic_u8string() + "/" + std::to_string(kind),
                                                 Ogre::GpuPageOutStrategy::Discard, kind, group);
    };
    // File textures repeat across their UVs and are filtered anisotropically.
    Ogre::HlmsSamplerblock tiled;
    tiled.setAddressingMode(Ogre::TAM_WRAP);
    tiled.setFiltering(Ogre::TFO_ANISOTROPIC);
    tiled.mMaxAnisotropy = 8;
    std::vector<Ogre::String> materialNames;
    for (std::size_t i = 0; i < parsed.materials.size(); ++i) {
        const auto& source = parsed.materials[i];
        const auto name = prefix + std::to_string(i) + "/" + source.name;
        Ogre::HlmsMacroblock macroblock;
        // Cut-out surfaces (foliage, signs) and glass are seen from both sides.
        if (source.alphaTested || source.blended) macroblock.mCullMode = Ogre::CULL_NONE;
        if (source.blended) macroblock.mDepthWrite = false;
        // A volume (queue 99) shows only its side facing the camera: a back face would absorb again.
        if (source.renderQueue == 99) macroblock.mCullMode = Ogre::CULL_CLOCKWISE;
        auto* material = static_cast<Ogre::HlmsPbsDatablock*>(
            pbs_.createDatablock(name, name, macroblock, Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        material->setWorkflow(Ogre::HlmsPbsDatablock::MetallicWorkflow);
        material->setDiffuse(Ogre::Vector3(1, 1, 1));
        material->setRoughness(1);
        material->setMetalness(1);
        // Clear dielectrics transmit instead of scattering: glass and liquids only reflect
        // (and tint through their absorption), or bright rooms would frost them.
        if (source.blended) material->setDiffuse(Ogre::Vector3::ZERO);
        else if (auto* base = texture(source.diffuse, Ogre::CommonTextureTypes::Diffuse))
            material->setTexture(Ogre::PBSM_DIFFUSE, base, &tiled);
        else material->setDiffuse(Ogre::Vector3(source.colour.x, source.colour.y, source.colour.z));
        if (source.alphaHashed) {
            // Kept with the probability of the diffuse alpha; temporal anti-aliasing softens it.
            material->setAlphaHashing(true);
            material->setTransparency(1, Ogre::HlmsPbsDatablock::None, true, false);
        } else if (source.alphaTested) {
            material->setAlphaTest(Ogre::CMPF_GREATER_EQUAL);
            material->setAlphaTestThreshold(0.5f);
        }
        if (source.renderQueue == 99) {
            Ogre::HlmsBlendblock multiply;
            multiply.mSourceBlendFactor = Ogre::SBF_ZERO;
            multiply.mDestBlendFactor = Ogre::SBF_SOURCE_COLOUR;
            material->setBlendblock(multiply);
        } else if (source.blended) {
            material->setTransparency(source.opacity, Ogre::HlmsPbsDatablock::Refractive, false);
            material->setRefractionStrength(.05f);
        }
        if (auto* normal = texture(source.normal, Ogre::CommonTextureTypes::NormalMap))
            material->setTexture(Ogre::PBSM_NORMAL, normal, &tiled);
        if (source.blended) {
            material->setRoughness(.05f);
            material->setMetalness(0);
        } else if (auto* orm = texture(source.orm, Ogre::CommonTextureTypes::NonColourData)) {
            material->setTexture(Ogre::PBSM_SPECULAR, orm, &tiled);
            material->setTexture(Ogre::PBSM_ROUGHNESS, orm, &tiled);
        } else {
            material->setRoughness(0.7f);
            material->setMetalness(0);
        }
        if (auto* emissive = texture(source.emissive, Ogre::CommonTextureTypes::Diffuse)) {
            material->setTexture(Ogre::PBSM_EMISSIVE, emissive, &tiled);
            material->setEmissive(Ogre::Vector3(source.emission));
        }
        // A datablock takes one custom piece file per stage. It is named after its content, so
        // materials with the same pieces share shaders (shadow casters included).
        if (!source.pieces.empty())
            material->setCustomPieceCodeFromMemory("Throne/" + std::to_string(std::hash<std::string>{}(source.pieces)), source.pieces,
                                                   Ogre::CustomPieceStage::PixelShader);
        material->setTexture(Ogre::PBSM_REFLECTION, reflection_);
        materialNames.push_back(name);
        out.materials.push_back(material);
    }

    auto* vaos = root_.getRenderSystem()->getVaoManager();
    const Ogre::VertexElement2Vec elements{{Ogre::VET_FLOAT3, Ogre::VES_POSITION},
                                           {Ogre::VET_FLOAT3, Ogre::VES_NORMAL},
                                           {Ogre::VET_FLOAT4, Ogre::VES_TANGENT},
                                           {Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES}};
    // A file mesh becomes one OGRE mesh per submesh, so every item has one material: items
    // can be shown a material at a time (see reveal()), and each draws in its material's
    // render queue, so opaque parts (which write depth) never draw in the refraction pass.
    std::vector<std::vector<std::pair<Ogre::uint8, Ogre::MeshPtr>>> meshes(parsed.meshes.size());
    for (std::size_t m = 0; m < parsed.meshes.size(); ++m) {
        const auto& submeshes = parsed.meshes[m].submeshes;
        for (std::size_t s = 0; s < submeshes.size(); ++s) {
            const auto& part = submeshes[s];
            const auto vertexCount = part.vertices.size() * sizeof(float) / sizeof(Vertex);
            auto mesh = Ogre::MeshManager::getSingleton().createManual(prefix + "mesh/" + std::to_string(m) + "/" + std::to_string(s),
                                                                        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            // OGRE copies immutable buffers on creation.
            auto* vertexBuffer = vaos->createVertexBuffer(elements, vertexCount, Ogre::BT_IMMUTABLE,
                                                          const_cast<float*>(part.vertices.data()), false);
            auto* indexBuffer = vaos->createIndexBuffer(Ogre::IndexBufferPacked::IT_32BIT, part.indices.size(), Ogre::BT_IMMUTABLE,
                                                        const_cast<std::uint32_t*>(part.indices.data()), false);
            auto* vao = vaos->createVertexArrayObject({vertexBuffer}, indexBuffer, Ogre::OT_TRIANGLE_LIST);
            auto* submesh = mesh->createSubMesh();
            submesh->mVao[Ogre::VpNormal].push_back(vao);
            submesh->mVao[Ogre::VpShadow].push_back(vao);
            if (part.material < materialNames.size()) submesh->setMaterialName(materialNames[part.material]);
            Vec3 lower{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
            Vec3 upper = lower * -1.0f;
            for (std::size_t v = 0; v < vertexCount; ++v) {
                const float* p = part.vertices.data() + v * (sizeof(Vertex) / sizeof(float));
                lower = {std::min(lower.x, p[0]), std::min(lower.y, p[1]), std::min(lower.z, p[2])};
                upper = {std::max(upper.x, p[0]), std::max(upper.y, p[1]), std::max(upper.z, p[2])};
            }
            const auto centre = (lower + upper) * 0.5f, half = (upper - lower) * 0.5f;
            mesh->_setBounds(Ogre::Aabb(Ogre::Vector3(centre.x, centre.y, centre.z), Ogre::Vector3(half.x, half.y, half.z)), false);
            mesh->_setBoundingSphereRadius(std::sqrt(dot(half, half)));
            const Ogre::uint8 queue = part.material < parsed.materials.size() ? parsed.materials[part.material].renderQueue : 0;
            meshes[m].emplace_back(queue, mesh);
            out.meshes.push_back(mesh);
        }
    }
    for (const auto& source : parsed.parts)
        for (const auto& [queue, mesh] : meshes[source.mesh]) {
            Part part;
            part.mesh = mesh;
            part.renderQueue = queue;
            part.position = source.position, part.scale = source.scale;
            std::copy(std::begin(source.rotation), std::end(source.rotation), part.rotation);
            out.parts.push_back(part);
        }
    models_.emplace(key, std::move(out));
}
}
