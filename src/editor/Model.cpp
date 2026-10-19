#include "editor/Model.h"
#include <OgreHlmsPbs.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreItem.h>
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
#include <limits>

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
}

ModelLibrary::ModelLibrary(Ogre::Root& root, Ogre::SceneManager& scene, Ogre::HlmsPbs& pbs, Log log)
    : root_(root), scene_(scene), pbs_(pbs), log_(std::move(log)) {}

ModelLibrary::~ModelLibrary() {
    for (auto& [key, model] : models_) {
        for (auto& mesh : model.meshes) Ogre::MeshManager::getSingleton().remove(mesh);
        for (auto* material : model.materials) pbs_.destroyDatablock(material->getName());
    }
}

ModelLibrary::Placed ModelLibrary::place(const std::filesystem::path& file, bool directXNormals,
                                         Ogre::SceneNode& parent) {
    const auto key = file.generic_u8string() + (directXNormals ? "#dx" : "#gl");
    auto found = models_.find(key);
    if (found == models_.end()) {
        Model model;
        if (!load(file, directXNormals, model)) return {};
        found = models_.emplace(key, std::move(model)).first;
    }
    const auto& model = found->second;
    Placed placed{parent.createChildSceneNode(), {}, model.lower, model.upper};
    for (const auto& part : model.parts) {
        auto* node = placed.root->createChildSceneNode();
        node->setPosition(part.position.x, part.position.y, part.position.z);
        node->setOrientation(Ogre::Quaternion(part.rotation[3], part.rotation[0], part.rotation[1], part.rotation[2]));
        node->setScale(part.scale.x, part.scale.y, part.scale.z);
        auto* item = scene_.createItem(part.mesh);
        item->setCastShadows(part.castShadows);
        if (part.refractive) item->setRenderQueueGroup(200u);
        node->attachObject(item);
        placed.items.push_back(item);
    }
    return placed;
}

void ModelLibrary::remove(Placed& placed) {
    for (auto* item : placed.items) scene_.destroyItem(item);
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

bool ModelLibrary::load(const std::filesystem::path& file, bool directXNormals, Model& out) {
    ufbx_load_opts options = {};
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0f;
    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(file.u8string().c_str(), &options, &error);
    if (!scene) {
        log_("Cannot read model " + file.u8string() + ": " + error.description.data);
        return false;
    }
    const auto folder = file.parent_path();
    const auto key = file.generic_u8string();
    auto& resources = Ogre::ResourceGroupManager::getSingleton();
    auto* textures = root_.getRenderSystem()->getTextureGpuManager();
    // Textures are found through a resource group per folder, named after the folder.
    const auto texture = [&](const ufbx_texture* source, Ogre::CommonTextureTypes::CommonTextureTypes kind,
                             bool* alpha) -> Ogre::TextureGpu* {
        if (!source) return nullptr;
        std::string relative(source->relative_filename.data, source->relative_filename.length);
        std::replace(relative.begin(), relative.end(), '\\', '/');
        auto path = folder / std::filesystem::u8path(relative);
        if (!std::filesystem::exists(path)) path = folder / "Textures" / std::filesystem::u8path(relative).filename();
        if (!std::filesystem::exists(path)) {
            log_("Model texture not found: " + path.u8string());
            return nullptr;
        }
        if (alpha) *alpha = ddsHasAlpha(path);
        const auto location = path.parent_path().generic_u8string();
        const Ogre::String group = "ThroneModels/" + location;
        if (!resources.resourceGroupExists(group)) resources.createResourceGroup(group);
        if (!resources.resourceLocationExists(location, group)) resources.addResourceLocation(location, "FileSystem", group);
        return textures->createOrRetrieveTexture(path.filename().u8string(),
                                                 path.generic_u8string() + "/" + std::to_string(kind),
                                                 Ogre::GpuPageOutStrategy::Discard, kind, group);
    };

    // File textures repeat across their UVs and are filtered anisotropically.
    Ogre::HlmsSamplerblock tiled;
    tiled.setAddressingMode(Ogre::TAM_WRAP);
    tiled.setFiltering(Ogre::TFO_ANISOTROPIC);
    tiled.mMaxAnisotropy = 8;
    // One PBS datablock per material of the file.
    std::vector<Ogre::String> materialNames(scene->materials.count);
    std::vector<bool> blendedMaterials(scene->materials.count), refractiveMaterials(scene->materials.count);
    for (std::size_t i = 0; i < scene->materials.count; ++i) {
        const auto* source = scene->materials.data[i];
        const auto name = "ThroneModel/" + key + "/" + std::to_string(i) + "/" + source->name.data;
        bool alpha = false;
        auto* base = texture(source->fbx.diffuse_color.texture, Ogre::CommonTextureTypes::Diffuse, &alpha);
        // The file's transparency colour makes glass and liquids blended; they keep their reflections.
        const auto& clear = source->fbx.transparency_color.value_vec3;
        const float opacity = 1.0f - float(source->fbx.transparency_factor.value_real * (clear.x + clear.y + clear.z) / 3);
        const bool blended = opacity < .99f;
        alpha &= !blended;
        Ogre::HlmsMacroblock macroblock;
        // Cut-out surfaces (foliage, signs) and glass are seen from both sides.
        if (alpha || blended) macroblock.mCullMode = Ogre::CULL_NONE;
        if (blended) macroblock.mDepthWrite = false;
        auto* material = static_cast<Ogre::HlmsPbsDatablock*>(
            pbs_.createDatablock(name, name, macroblock, Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        material->setWorkflow(Ogre::HlmsPbsDatablock::MetallicWorkflow);
        material->setDiffuse(Ogre::Vector3(1, 1, 1));
        material->setRoughness(1);
        material->setMetalness(1);
        if (base) material->setTexture(Ogre::PBSM_DIFFUSE, base, &tiled);
        else {
            const auto& colour = source->fbx.diffuse_color.value_vec3;
            material->setDiffuse(Ogre::Vector3(float(colour.x), float(colour.y), float(colour.z)));
        }
        if (alpha) {
            material->setAlphaTest(Ogre::CMPF_GREATER_EQUAL);
            material->setAlphaTestThreshold(0.5f);
        }
        // Nearly clear glass refracts the scene drawn before it; denser liquids and bottles
        // blend and are drawn with that scene, so glass in front of them still shows them.
        const bool refracts = blended && opacity < .25f;
        if (refracts) {
            material->setTransparency(opacity, Ogre::HlmsPbsDatablock::Refractive, false);
            material->setRefractionStrength(.05f);
        } else if (blended)
            material->setTransparency(opacity, Ogre::HlmsPbsDatablock::Transparent, false);
        if (auto* normal = texture(source->fbx.normal_map.texture, Ogre::CommonTextureTypes::NormalMap, nullptr))
            material->setTexture(Ogre::PBSM_NORMAL, normal, &tiled);
        if (auto* orm = texture(source->fbx.specular_color.texture, Ogre::CommonTextureTypes::NonColourData, nullptr)) {
            material->setTexture(Ogre::PBSM_SPECULAR, orm, &tiled);
            material->setTexture(Ogre::PBSM_ROUGHNESS, orm, &tiled);
            material->setCustomPieceCodeFromMemory("ThroneOrm", ormPiece, Ogre::CustomPieceStage::PixelShader);
        } else {
            material->setRoughness(0.7f);
            material->setMetalness(0);
        }
        if (auto* emissive = texture(source->fbx.emission_color.texture, Ogre::CommonTextureTypes::Diffuse, nullptr)) {
            material->setTexture(Ogre::PBSM_EMISSIVE, emissive, &tiled);
            material->setEmissive(Ogre::Vector3(1, 1, 1));
        }
        materialNames[i] = name;
        blendedMaterials[i] = blended;
        refractiveMaterials[i] = refracts;
        material->setTexture(Ogre::PBSM_REFLECTION, reflection_);
        out.materials.push_back(material);
    }

    // One OGRE mesh per file mesh, one submesh per material it uses.
    auto* vaos = root_.getRenderSystem()->getVaoManager();
    const Ogre::VertexElement2Vec elements{{Ogre::VET_FLOAT3, Ogre::VES_POSITION},
                                           {Ogre::VET_FLOAT3, Ogre::VES_NORMAL},
                                           {Ogre::VET_FLOAT4, Ogre::VES_TANGENT},
                                           {Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES}};
    std::map<const ufbx_mesh*, Ogre::MeshPtr> meshes;
    std::map<const ufbx_mesh*, bool> opaqueMeshes, refractiveMeshes;
    std::vector<uint32_t> triangle;
    for (std::size_t m = 0; m < scene->meshes.count; ++m) {
        const auto* source = scene->meshes.data[m];
        if (!source->vertex_position.exists || source->num_triangles == 0) continue;
        auto mesh = Ogre::MeshManager::getSingleton().createManual("ThroneModel/" + key + "/" + std::to_string(m),
                                                                    Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        Vec3 lower{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        Vec3 upper = lower * -1.0f;
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
            std::vector<uint32_t> indices(corners.size());
            ufbx_vertex_stream stream{corners.data(), corners.size(), sizeof(Corner)};
            const auto unique = ufbx_generate_indices(&stream, 1, indices.data(), indices.size(), nullptr, &error);
            if (error.type != UFBX_ERROR_NONE) continue;
            corners.resize(unique);
            std::vector<Vertex> vertices;
            deriveTangents(corners, indices, directXNormals, vertices);
            for (const auto& v : vertices) {
                lower = {std::min(lower.x, v.position[0]), std::min(lower.y, v.position[1]), std::min(lower.z, v.position[2])};
                upper = {std::max(upper.x, v.position[0]), std::max(upper.y, v.position[1]), std::max(upper.z, v.position[2])};
            }
            // OGRE copies immutable buffers on creation, so the arrays are freed here.
            auto* vertexBuffer = vaos->createVertexBuffer(elements, vertices.size(), Ogre::BT_IMMUTABLE, vertices.data(), false);
            auto* indexBuffer = vaos->createIndexBuffer(Ogre::IndexBufferPacked::IT_32BIT, indices.size(),
                                                        Ogre::BT_IMMUTABLE, indices.data(), false);
            auto* vao = vaos->createVertexArrayObject({vertexBuffer}, indexBuffer, Ogre::OT_TRIANGLE_LIST);
            auto* submesh = mesh->createSubMesh();
            submesh->mVao[Ogre::VpNormal].push_back(vao);
            submesh->mVao[Ogre::VpShadow].push_back(vao);
            const auto* material = part.index < source->materials.count ? source->materials.data[part.index] : nullptr;
            if (material) submesh->setMaterialName(materialNames[material->typed_id]);
            opaqueMeshes[source] = opaqueMeshes[source] || !material || !blendedMaterials[material->typed_id];
            refractiveMeshes[source] = refractiveMeshes[source] || (material && refractiveMaterials[material->typed_id]);
        }
        if (mesh->getNumSubMeshes() == 0) {
            Ogre::MeshManager::getSingleton().remove(mesh);
            continue;
        }
        const auto centre = (lower + upper) * 0.5f, half = (upper - lower) * 0.5f;
        mesh->_setBounds(Ogre::Aabb(Ogre::Vector3(centre.x, centre.y, centre.z), Ogre::Vector3(half.x, half.y, half.z)), false);
        mesh->_setBoundingSphereRadius(std::sqrt(dot(half, half)));
        meshes[source] = mesh;
        out.meshes.push_back(mesh);
    }

    // Every node with a mesh places it; the bounds gather the placed boxes' corners.
    out.lower = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    out.upper = out.lower * -1.0f;
    for (std::size_t n = 0; n < scene->nodes.count; ++n) {
        const auto* node = scene->nodes.data[n];
        const auto found = node->mesh ? meshes.find(node->mesh) : meshes.end();
        if (found == meshes.end()) continue;
        const auto transform = ufbx_matrix_to_transform(&node->geometry_to_world);
        Part part;
        part.mesh = found->second;
        part.castShadows = opaqueMeshes[node->mesh];
        part.refractive = refractiveMeshes[node->mesh];
        part.position = vec(transform.translation);
        part.scale = vec(transform.scale);
        part.rotation[0] = float(transform.rotation.x), part.rotation[1] = float(transform.rotation.y);
        part.rotation[2] = float(transform.rotation.z), part.rotation[3] = float(transform.rotation.w);
        const auto aabb = found->second->getAabb();
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 local{aabb.mCenter.x + (corner & 1 ? aabb.mHalfSize.x : -aabb.mHalfSize.x),
                             aabb.mCenter.y + (corner & 2 ? aabb.mHalfSize.y : -aabb.mHalfSize.y),
                             aabb.mCenter.z + (corner & 4 ? aabb.mHalfSize.z : -aabb.mHalfSize.z)};
            const auto world = vec(ufbx_transform_position(&node->geometry_to_world, {local.x, local.y, local.z}));
            out.lower = {std::min(out.lower.x, world.x), std::min(out.lower.y, world.y), std::min(out.lower.z, world.z)};
            out.upper = {std::max(out.upper.x, world.x), std::max(out.upper.y, world.y), std::max(out.upper.z, world.z)};
        }
        out.parts.push_back(part);
    }
    ufbx_free_scene(scene);
    if (out.parts.empty()) {
        log_("Model " + file.u8string() + " has no meshes");
        out.lower = out.upper = {};
    }
    return true;
}
}
