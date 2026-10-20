#include "editor/GlobalIllumination.h"
#include "editor/Lton.h"
#include <CommandBuffer/OgreCbTexture.h>
#include <CommandBuffer/OgreCommandBuffer.h>
#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Cubemaps/OgreCubemapProbe.h>
#include <Cubemaps/OgreParallaxCorrectedCubemapAuto.h>
#include <OgreAsyncTextureTicket.h>
#include <OgreBitwise.h>
#include <OgreCamera.h>
#include <OgreDataStream.h>
#include <OgreHlmsListener.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsPbs.h>
#include <OgreId.h>
#include <OgreImage2.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreTextureGpu.h>
#include <OgreTextureGpuManager.h>
#include <cmath>
#include <fstream>
#include <iterator>
#include <vector>

namespace throne {
namespace {
constexpr const char* giFormat = "ogrethrone-gi";
/// 2: the Adaptive Irradiance Field instead of OGRE's irradiance field.
constexpr LhatInteger giVersion = 2;
/// Reflection probe cubemaps: edge in texels and format.
constexpr Ogre::uint32 probeResolution = 256;
constexpr Ogre::PixelFormatGpu probeFormat = Ogre::PFG_RGBA16_FLOAT;
constexpr float probeNear = .05f, probeFar = 500;
/// Diffuse probe cubemaps: edge in texels, and how many may be waiting to be read back.
constexpr Ogre::uint32 diffuseResolution = 32;
constexpr std::size_t diffuseInFlight = 32;
/// The width of the diffuse light's 2D tables; rows follow on.
constexpr Ogre::uint32 tableWidth = 4096;
/// How camera_cubemap_reorient turns the camera for each face (OGRE's protected
/// CompositorPass::CubemapRotations): +X, -X, +Y, -Y, +Z, -Z.
const Ogre::Quaternion cubemapRotations[6] = {
    Ogre::Quaternion(Ogre::Radian(-1.570796f), Ogre::Vector3(0, 1, 0)), Ogre::Quaternion(Ogre::Radian(1.570796f), Ogre::Vector3(0, 1, 0)),
    Ogre::Quaternion(Ogre::Radian(1.570796f), Ogre::Vector3(1, 0, 0)),  Ogre::Quaternion(Ogre::Radian(-1.570796f), Ogre::Vector3(1, 0, 0)),
    Ogre::Quaternion(1, 0, 0, 0),                                        Ogre::Quaternion(Ogre::Radian(3.1415927f), Ogre::Vector3(0, 1, 0))};
/// The shader's names of the diffuse light's textures, in the order they are bound.
constexpr const char* diffuseTextureNames[4] = {"aifTop", "aifNodes", "aifCells", "aifProbes"};

Ogre::Vector3 toOgre(const Vec3& v) { return {v.x, v.y, v.z}; }
Vec3 fromOgre(const Ogre::Vector3& v) { return {float(v.x), float(v.y), float(v.z)}; }

/// Writes every mipmap and slice of @p texture to @p file.
void saveTexture(Ogre::TextureGpu& texture, const std::filesystem::path& file) {
    Ogre::Image2 image;
    image.convertFromTexture(&texture, 0, Ogre::uint8(texture.getNumMipmaps() - 1u));
    image.save(file.u8string(), 0, image.getNumMipmaps());
}

/// Reads @p file as saveTexture() wrote it.
std::shared_ptr<Ogre::Image2> readTexture(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<char> bytes(std::istreambuf_iterator<char>(in), {});
    if (bytes.empty()) throw std::runtime_error("Could not read " + file.u8string());
    Ogre::DataStreamPtr stream(OGRE_NEW Ogre::MemoryDataStream(bytes.data(), bytes.size(), false, true));
    auto image = std::make_shared<Ogre::Image2>();
    image->load(stream, "oitd");
    return image;
}

/// Uploads @p image into @p texture, which must have its size, format and mipmaps.
void uploadTexture(const Ogre::Image2& image, Ogre::TextureGpu& texture) {
    if (image.getWidth() != texture.getWidth() || image.getPixelFormat() != texture.getPixelFormat() ||
        image.getNumMipmaps() != texture.getNumMipmaps())
        throw std::runtime_error("A baked texture of " + texture.getNameStr() + " does not match it");
    const_cast<Ogre::Image2&>(image).uploadTo(&texture, 0, Ogre::uint8(image.getNumMipmaps() - 1u));
}

/// A new texture holding @p image, read by the shader with texelFetch.
Ogre::TextureGpu* textureFrom(Ogre::TextureGpuManager& textures, const Ogre::Image2& image, const std::string& name) {
    auto* texture = textures.createTexture(name, Ogre::GpuPageOutStrategy::Discard, Ogre::TextureFlags::ManualTexture,
                                           image.getTextureType());
    texture->setResolution(image.getWidth(), image.getHeight(), image.getDepthOrSlices());
    texture->setPixelFormat(image.getPixelFormat());
    texture->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    texture->_setNextResidencyStatus(Ogre::GpuResidency::Resident);
    const_cast<Ogre::Image2&>(image).uploadTo(texture, 0, 0);
    return texture;
}

/// A 2D table of @p count entries of @p perEntry texels, tableWidth texels a row; @p fill
/// writes entry i's texels at the pointer it gets.
template <class Texel, class Fill>
std::shared_ptr<Ogre::Image2> table(std::size_t count, unsigned perEntry, Ogre::PixelFormatGpu format, Fill fill) {
    const std::size_t texels = std::max<std::size_t>(1, count * perEntry);
    auto image = std::make_shared<Ogre::Image2>();
    image->createEmptyImage(tableWidth, Ogre::uint32((texels + tableWidth - 1) / tableWidth), 1, Ogre::TextureTypes::Type2D,
                            format);
    const auto box = image->getData(0);
    std::vector<Texel> row(tableWidth * perEntry);
    for (std::size_t i = 0; i < count; ++i) {
        std::vector<Texel> entry(perEntry);
        fill(i, entry.data());
        for (unsigned k = 0; k < perEntry; ++k) {
            const std::size_t at = i * perEntry + k;
            *static_cast<Texel*>(box.at(at % tableWidth, at / tableWidth, 0)) = entry[k];
        }
    }
    return image;
}

/// Packs @p octree and its probes' @p values into the diffuse light's images (TDDja/12 3).
GlobalIllumination::Diffuse pack(const ProbeOctree& octree, const std::vector<std::array<float, 12>>& values) {
    GlobalIllumination::Diffuse out;
    out.lower = octree.lower;
    out.spacing = octree.spacing;
    out.top = octree.top;
    out.topNodes = std::make_shared<Ogre::Image2>();
    out.topNodes->createEmptyImage(octree.top[0], octree.top[1], octree.top[2], Ogre::TextureTypes::Type3D,
                                   Ogre::PFG_R32_UINT);
    const auto box = out.topNodes->getData(0);
    for (std::uint32_t z = 0; z < octree.top[2]; ++z)
        for (std::uint32_t y = 0; y < octree.top[1]; ++y)
            for (std::uint32_t x = 0; x < octree.top[0]; ++x)
                *static_cast<std::uint32_t*>(box.at(x, y, z)) =
                    octree.topNodes[(std::size_t(z) * octree.top[1] + y) * octree.top[0] + x];
    using U4 = std::array<std::uint32_t, 4>;
    using F4 = std::array<float, 4>;
    out.nodes = table<std::uint32_t>(octree.nodes.size(), 1, Ogre::PFG_R32_UINT,
                                     [&](std::size_t i, std::uint32_t* to) { *to = octree.nodes[i]; });
    out.cells = table<U4>(octree.cells.size(), 2, Ogre::PFG_RGBA32_UINT, [&](std::size_t i, U4* to) {
        const auto& c = octree.cells[i].corners;
        to[0] = {c[0], c[1], c[2], c[3]};
        to[1] = {c[4], c[5], c[6], c[7]};
    });
    out.probes = table<F4>(values.size(), 3, Ogre::PFG_RGBA32_FLOAT, [&](std::size_t i, F4* to) {
        for (unsigned k = 0; k < 3; ++k) to[k] = {values[i][k * 4], values[i][k * 4 + 1], values[i][k * 4 + 2], values[i][k * 4 + 3]};
    });
    return out;
}

/// The settings fields a bake records, as written to and read from gi.lton.
void writeSettings(lton::Writer& out, LhatValue table, const GiSettings& s) {
    out.set(table, "lower", out.vector(s.lower.data()));
    out.set(table, "upper", out.vector(s.upper.data()));
    const float reflections[3] = {float(s.reflectionProbes[0]), float(s.reflectionProbes[1]), float(s.reflectionProbes[2])};
    out.set(table, "reflectionProbes", out.vector(reflections));
    out.set(table, "bounces", lhat_integer(s.bounces));
    out.set(table, "probeSpacing", lhat_real(s.probeSpacing));
    out.set(table, "minObjectVolume", lhat_real(s.minObjectVolume));
    out.set(table, "fillEmptySpaces", lhat_bool(s.fillEmptySpaces));
    out.set(table, "localProbeVolume", lhat_bool(s.localProbeVolume));
    out.set(table, "fastBakeShadows", lhat_bool(s.fastBakeShadows));
}
GiSettings readSettings(const LhatTable& table) {
    GiSettings s;
    s.enabled = true;
    lton::vector(lton::field(table, "lower"), s.lower.data());
    lton::vector(lton::field(table, "upper"), s.upper.data());
    float reflections[3];
    lton::vector(lton::field(table, "reflectionProbes"), reflections);
    for (int i = 0; i < 3; ++i) s.reflectionProbes[i] = unsigned(reflections[i]);
    s.bounces = unsigned(lton::integer(lton::field(table, "bounces")));
    s.probeSpacing = float(lton::number(lton::field(table, "probeSpacing")));
    s.minObjectVolume = float(lton::number(lton::field(table, "minObjectVolume")));
    s.fillEmptySpaces = lhat_as_bool(lton::field(table, "fillEmptySpaces"));
    s.localProbeVolume = lhat_as_bool(lton::field(table, "localProbeVolume"));
    // Bakes from before the setting used the full shadows.
    const auto fast = lton::field(table, "fastBakeShadows");
    s.fastBakeShadows = !lhat_is_nil(fast) && lhat_as_bool(fast);
    return s;
}
}

/// Hands PBS the diffuse light (TDDja/12 5), as OGRE's Terra hands it terrain shadows: sets
/// custom_diffuse_gi, binds four textures after PBS's own pass textures and writes the octree's
/// placement into the pass buffer, all in passes that are not shadow casters.
class GlobalIllumination::Listener final : public Ogre::HlmsListener {
public:
    /// The textures to bind (none while there is no diffuse light) and where the octree lies.
    std::array<Ogre::TextureGpu*, 4> textures{};
    Vec3 lower;
    float spacing = 1;
    std::array<std::uint32_t, 3> top{};

    explicit Listener(Ogre::HlmsManager& manager) : manager_(manager) {
        Ogre::HlmsSamplerblock point;
        point.mMinFilter = point.mMagFilter = Ogre::FO_POINT;
        point.mMipFilter = Ogre::FO_NONE;
        sampler_ = manager.getSamplerblock(point);
    }
    ~Listener() { manager_.destroySamplerblock(sampler_); }

    Ogre::uint16 getNumExtraPassTextures(const Ogre::HlmsPropertyVec& properties, bool casterPass) const override {
        return !casterPass && Ogre::Hlms::getProperty(properties, property) ? 4u : 0u;
    }
    void propertiesMergedPreGenerationStep(Ogre::Hlms* hlms, const Ogre::HlmsCache&, const Ogre::HlmsPropertyVec&,
                                           const Ogre::PiecesMap[Ogre::NumShaderTypes], const Ogre::HlmsPropertyVec&,
                                           const Ogre::QueuedRenderable&, size_t tid) override {
        if (hlms->_getProperty(tid, Ogre::HlmsBaseProp::ShadowCaster) || !hlms->_getProperty(tid, property)) return;
        // The extra pass textures end PBS's first set of textures.
        auto unit = hlms->_getProperty(tid, Ogre::PbsProperty::Set0TextureSlotEnd) - 4;
        for (const auto* name : diffuseTextureNames) hlms->_setTextureReg(tid, Ogre::PixelShader, name, unit++);
    }
    void preparePassHash(const Ogre::CompositorShadowNode*, bool casterPass, bool, Ogre::SceneManager*,
                         Ogre::Hlms* hlms) override {
        if (!casterPass) hlms->_setProperty(Ogre::Hlms::kNoTid, property, textures[0] != nullptr);
    }
    Ogre::uint32 getPassBufferSize(const Ogre::CompositorShadowNode*, bool casterPass, bool, Ogre::SceneManager*) const override {
        return !casterPass && textures[0] ? 7u * 4u * 4u : 0u;
    }
    float* preparePassBuffer(const Ogre::CompositorShadowNode*, bool casterPass, bool, Ogre::SceneManager* sceneManager,
                             float* out) override {
        if (casterPass || !textures[0]) return out;
        // View space to finest cells from lower, then view to world directions (TDDja/12 5).
        const auto* camera = sceneManager->getCamerasInProgress().renderingCamera;
        const auto viewToWorld = camera->getVrViewMatrix(0).inverseAffine();
        Ogre::Matrix4 toGrid = Ogre::Matrix4::getScale(Ogre::Vector3(1 / spacing)) * Ogre::Matrix4::getTrans(-toOgre(lower)) *
                               viewToWorld;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column) *out++ = float(toGrid[row][column]);
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column) *out++ = column < 3 ? float(viewToWorld[row][column]) : 0.0f;
        *out++ = float(top[0]);
        *out++ = float(top[1]);
        *out++ = float(top[2]);
        *out++ = float(ProbeOctree::topCells);
        return out;
    }
    void hlmsTypeChanged(bool casterPass, Ogre::CommandBuffer* commands, const Ogre::HlmsDatablock*, size_t unit) override {
        if (casterPass || !textures[0]) return;
        for (auto* texture : textures) *commands->addCommand<Ogre::CbTexture>() = Ogre::CbTexture(Ogre::uint16(unit++), texture, sampler_);
    }

private:
    static inline const Ogre::IdString property{"custom_diffuse_gi"};
    Ogre::HlmsManager& manager_;
    const Ogre::HlmsSamplerblock* sampler_ = nullptr;
};

/// Renders diffuse probes' cubemaps and reads them back, a few frames later, as spherical
/// harmonics (TDDja/12 4).
class GlobalIllumination::ProbeRenderer {
public:
    /// @param fastShadows Renders with ThroneGi/FastShadows (GiSettings::fastBakeShadows).
    ProbeRenderer(Ogre::Root& root, Ogre::SceneManager& scene, float far, bool fastShadows) : root_(root), scene_(scene) {
        auto& textures = *root.getRenderSystem()->getTextureGpuManager();
        const auto id = std::to_string(Ogre::Id::generateNewId<ProbeRenderer>());
        cubemap_ = textures.createTexture("ThroneGi/Diffuse/" + id, Ogre::GpuPageOutStrategy::Discard,
                                          Ogre::TextureFlags::RenderToTexture, Ogre::TextureTypes::TypeCube);
        cubemap_->setResolution(diffuseResolution, diffuseResolution);
        cubemap_->setPixelFormat(probeFormat);
        cubemap_->scheduleTransitionTo(Ogre::GpuResidency::Resident);
        // The camera keeps its orientation; the passes turn it to each face (CubemapRotations).
        camera_ = scene.createCamera("ThroneGi/Diffuse/" + id, true, true);
        camera_->setFOVy(Ogre::Degree(90));
        camera_->setAspectRatio(1);
        camera_->setFixedYawAxis(false);
        camera_->setNearClipDistance(.02f);
        camera_->setFarClipDistance(far);
        workspace_ = root.getCompositorManager2()->addWorkspace(
            &scene, cubemap_, camera_, fastShadows ? "ThroneGi/AdaptiveFastWorkspace" : "ThroneGi/AdaptiveWorkspace", false);
        for (std::size_t i = 0; i < diffuseInFlight; ++i)
            free_.push_back(textures.createAsyncTextureTicket(diffuseResolution, diffuseResolution, 6, Ogre::TextureTypes::TypeCube,
                                                              probeFormat));
        // Each texel's direction (from the face's camera turn) and solid angle.
        texels_.reserve(6 * diffuseResolution * diffuseResolution);
        for (unsigned face = 0; face < 6; ++face)
            for (unsigned y = 0; y < diffuseResolution; ++y)
                for (unsigned x = 0; x < diffuseResolution; ++x) {
                    const float u = (float(x) + .5f) / float(diffuseResolution) * 2 - 1;
                    const float v = 1 - (float(y) + .5f) / float(diffuseResolution) * 2;
                    const auto direction = (cubemapRotations[face] * Ogre::Vector3(u, v, -1)).normalisedCopy();
                    const float solidAngle = 4.0f / float(diffuseResolution * diffuseResolution) / std::pow(u * u + v * v + 1, 1.5f);
                    texels_.push_back({fromOgre(direction), solidAngle});
                }
    }
    ~ProbeRenderer() {
        auto& textures = *root_.getRenderSystem()->getTextureGpuManager();
        for (auto& [ticket, probe] : inFlight_) free_.push_back(ticket);
        for (auto* ticket : free_) textures.destroyAsyncTextureTicket(ticket);
        root_.getCompositorManager2()->removeWorkspace(workspace_);
        scene_.destroyCamera(camera_);
        textures.destroyTexture(cubemap_);
    }

    /// Whether another probe can be rendered before some are read back.
    bool ready() const { return !free_.empty(); }
    /// Renders probe @p probe's cubemap at @p at and starts reading it back.
    void render(const Vec3& at, std::uint32_t probe) {
        camera_->setPosition(toOgre(at));
        workspace_->_validateFinalTarget();
        workspace_->_beginUpdate(false);
        workspace_->_update();
        workspace_->_endUpdate(false);
        auto* ticket = free_.back();
        free_.pop_back();
        // The copy runs after the rendering, so the cubemap can be drawn into again at once.
        ticket->download(cubemap_, 0, false);
        inFlight_.push_back({ticket, probe});
    }
    /// Projects every cubemap read back so far and passes each probe and its values to @p done.
    template <class Done>
    void collect(Done done) {
        for (std::size_t i = 0; i < inFlight_.size();) {
            auto [ticket, probe] = inFlight_[i];
            if (!ticket->queryIsTransferDone()) {
                ++i;
                continue;
            }
            done(probe, project(*ticket));
            free_.push_back(ticket);
            inFlight_.erase(inFlight_.begin() + std::ptrdiff_t(i));
        }
    }

private:
    /// The light along a normal n, a + b.n per colour, from the radiance L seen: the cosine lobe
    /// over L1 spherical harmonics gives a = Integral(L)/(4 pi) and b = Integral(L w)/(2 pi), as
    /// irradiance / pi (what PBS's diffuse environment term expects).
    std::array<float, 12> project(Ogre::AsyncTextureTicket& ticket) {
        const auto box = ticket.map(0);
        double sums[3][4] = {};
        std::size_t texel = 0;
        for (unsigned face = 0; face < 6; ++face)
            for (unsigned y = 0; y < diffuseResolution; ++y)
                for (unsigned x = 0; x < diffuseResolution; ++x, ++texel) {
                    const auto* rgba = static_cast<const Ogre::uint16*>(box.at(x, y, face));
                    const auto& [direction, solidAngle] = texels_[texel];
                    for (int c = 0; c < 3; ++c) {
                        const double radiance = double(Ogre::Bitwise::halfToFloat(rgba[c])) * solidAngle;
                        sums[c][0] += radiance;
                        sums[c][1] += radiance * direction.x;
                        sums[c][2] += radiance * direction.y;
                        sums[c][3] += radiance * direction.z;
                    }
                }
        ticket.unmap();
        constexpr double pi = 3.14159265358979;
        std::array<float, 12> out{};
        for (int c = 0; c < 3; ++c) {
            out[std::size_t(c * 4)] = float(sums[c][0] / (4 * pi));
            for (int k = 1; k < 4; ++k) out[std::size_t(c * 4 + k)] = float(sums[c][k] / (2 * pi));
        }
        return out;
    }

    Ogre::Root& root_;
    Ogre::SceneManager& scene_;
    Ogre::TextureGpu* cubemap_ = nullptr;
    Ogre::Camera* camera_ = nullptr;
    Ogre::CompositorWorkspace* workspace_ = nullptr;
    std::vector<Ogre::AsyncTextureTicket*> free_;
    std::vector<std::pair<Ogre::AsyncTextureTicket*, std::uint32_t>> inFlight_;
    std::vector<std::pair<Vec3, float>> texels_;
};

GlobalIllumination::GlobalIllumination(Ogre::Root& root, Ogre::SceneManager& scene, Ogre::HlmsPbs& pbs, Log log)
    : root_(root), scene_(scene), pbs_(pbs), log_(std::move(log)),
      listener_(std::make_unique<Listener>(*root.getHlmsManager())) {
    pbs_.setListener(listener_.get());
}

GlobalIllumination::~GlobalIllumination() {
    clear();
    pbs_.setListener(nullptr);
}

bool GlobalIllumination::baking() const { return pending_.reflections != nullptr; }

void GlobalIllumination::clear() {
    pending_ = {};
    pbs_.setParallaxCorrectedCubemap(nullptr);
    reflections_.reset();
    destroyDiffuseTextures();
    diffuse_ = {};
    ++version_;
}

void GlobalIllumination::destroyDiffuseTextures() {
    listener_->textures = {};
    auto& textures = *root_.getRenderSystem()->getTextureGpuManager();
    for (auto*& texture : diffuseTextures_)
        if (texture) textures.destroyTexture(texture), texture = nullptr;
}

std::vector<std::pair<Vec3, Vec3>> GlobalIllumination::reflectionAreas() const {
    std::vector<std::pair<Vec3, Vec3>> areas;
    if (reflections_)
        for (const auto* probe : reflections_->getProbes()) {
            const auto& area = probe->getArea();
            areas.emplace_back(Vec3{area.mCenter.x, area.mCenter.y, area.mCenter.z},
                               Vec3{area.mHalfSize.x, area.mHalfSize.y, area.mHalfSize.z});
        }
    return areas;
}

std::unique_ptr<Ogre::ParallaxCorrectedCubemapAuto> GlobalIllumination::createReflections(unsigned probes) {
    const auto* workspace = root_.getCompositorManager2()->getWorkspaceDefinition("ThroneGi/ProbeWorkspace");
    auto reflections = std::make_unique<Ogre::ParallaxCorrectedCubemapAuto>(
        Ogre::Id::generateNewId<Ogre::ParallaxCorrectedCubemapAuto>(), &root_, &scene_, workspace);
    reflections->setEnabled(true, probeResolution, probeResolution, probes, probeFormat);
    return reflections;
}

void GlobalIllumination::use(const GiSettings& settings, std::unique_ptr<Ogre::ParallaxCorrectedCubemapAuto> reflections,
                             const Diffuse& diffuse) {
    clear();
    settings_ = settings;
    reflections_ = std::move(reflections);
    pbs_.setParallaxCorrectedCubemap(reflections_.get());
    diffuse_ = diffuse;
    auto& textures = *root_.getRenderSystem()->getTextureGpuManager();
    const auto id = std::to_string(version_);
    const Ogre::Image2* images[4] = {diffuse.topNodes.get(), diffuse.nodes.get(), diffuse.cells.get(), diffuse.probes.get()};
    for (int i = 0; i < 4; ++i)
        diffuseTextures_[std::size_t(i)] =
            textureFrom(textures, *images[i], "ThroneGi/" + id + "/" + diffuseTextureNames[i]);
    listener_->textures = diffuseTextures_;
    listener_->lower = diffuse.lower;
    listener_->spacing = diffuse.spacing;
    listener_->top = diffuse.top;
    ++version_;
}

void GlobalIllumination::beginBake(const GiSettings& settings, Ogre::Camera& camera, ProbeOctree octree) {
    const auto& counts = settings.reflectionProbes;
    auto reflections = createReflections(counts[0] * counts[1] * counts[2]);
    reflections->setUpdatedTrackedDataFromCamera(&camera);
    // Rendered only by continueBake(): the bake is the scene as it was then.
    reflections->mPaused = true;
    pending_ = {};
    pending_.settings = settings;
    pending_.reflections = std::move(reflections);
    for (std::uint32_t i = 0; i < octree.probes.size(); ++i)
        if (octree.probes[i].from < 0) pending_.toRender.push_back(i);
    pending_.values.resize(octree.probes.size());
    // Probes see twice across the octree.
    const auto extent = Vec3{float(octree.top[0]), float(octree.top[1]), float(octree.top[2])} *
                        (octree.spacing * float(ProbeOctree::topCells));
    pending_.renderer = std::make_unique<ProbeRenderer>(root_, scene_, std::max(1.0f, 2 * std::sqrt(extent.x * extent.x + extent.y * extent.y + extent.z * extent.z)),
                                                        settings.fastBakeShadows);
    pending_.octree = std::move(octree);
}

void GlobalIllumination::renderNextReflectionProbe() {
    // The probes stand on a grid over the diffuse probes' volume, each reaching over its cell by
    // a quarter, and are parallax-corrected against the whole volume.
    const auto& counts = pending_.settings.reflectionProbes;
    const auto i = pending_.reflectionProbesDone;
    const Ogre::Vector3 cell((i % counts[0]) + .5f, ((i / counts[0]) % counts[1]) + .5f, (i / (counts[0] * counts[1])) + .5f);
    const auto lower = toOgre(pending_.octree.lower), size = toOgre(pending_.octree.upper - pending_.octree.lower);
    const Ogre::Vector3 numbers{float(counts[0]), float(counts[1]), float(counts[2])};
    const auto centre = lower + size * cell / numbers;
    Ogre::Aabb region;
    region.setExtents(lower, lower + size);
    auto* probe = pending_.reflections->createProbe();
    probe->setTextureParams(probeResolution, probeResolution);
    probe->initWorkspace(probeNear, probeFar);
    probe->set(centre, Ogre::Aabb(centre, size * .5f * 1.25f / numbers), Ogre::Vector3(.05f), Ogre::Matrix3::IDENTITY, region);
    probe->setStatic(true);
    // Renders the dirty probes, which is only the new one.
    pending_.reflections->updateAllDirtyProbes();
    ++pending_.reflectionProbesDone;
}

void GlobalIllumination::continueBake(std::chrono::milliseconds budget) {
    if (!baking()) return;
    const auto start = std::chrono::steady_clock::now();
    const auto& r = pending_.settings.reflectionProbes;
    if (pending_.reflectionProbesDone < r[0] * r[1] * r[2]) {
        do renderNextReflectionProbe();
        while (pending_.reflectionProbesDone < r[0] * r[1] * r[2] && std::chrono::steady_clock::now() - start < budget);
        return;
    }
    auto& renderer = *pending_.renderer;
    renderer.collect([&](std::uint32_t probe, const std::array<float, 12>& values) {
        pending_.values[probe] = values;
        ++pending_.collected;
    });
    if (pending_.sent == 0) pending_.diffuseStart = start;
    if (pending_.sent < pending_.toRender.size()) {
        // As OGRE's IrradianceFieldRaster: the frame is ended and begun again every few probes,
        // so one frame does not hold too many renderings.
        auto* system = root_.getRenderSystem();
        scene_.updateSceneGraph();
        for (unsigned n = 0; pending_.sent < pending_.toRender.size() && renderer.ready() &&
                             std::chrono::steady_clock::now() - start < budget;
             ++n) {
            if (n > 0 && n % 8 == 0) system->_beginFrameOnce();
            const auto probe = pending_.toRender[pending_.sent++];
            renderer.render(pending_.octree.position(pending_.octree.probes[probe]), probe);
            if (n > 0 && n % 8 == 0) {
                system->_update();
                system->_endFrameOnce();
            }
        }
        return;
    }
    if (pending_.collected < pending_.toRender.size()) return;
    pending_.octree.interpolateHanging(pending_.values);
    log_("Baked global illumination: " + std::to_string(pending_.reflections->getProbes().size()) + " reflection probes, " +
         std::to_string(pending_.toRender.size()) + " diffuse probes rendered, " +
         std::to_string(pending_.octree.probes.size() - pending_.toRender.size()) + " interpolated, in " +
         std::to_string(std::chrono::duration<double>(std::chrono::steady_clock::now() - pending_.diffuseStart).count()) +
         " s");
    const auto diffuse = pack(pending_.octree, pending_.values);
    // use() clears pending_, so the finished bake is moved out of it first.
    auto settings = pending_.settings;
    auto reflections = std::move(pending_.reflections);
    pending_ = {};
    use(settings, std::move(reflections), diffuse);
}

float GlobalIllumination::bakeProgress() const {
    if (!baking()) return 1;
    const auto& r = pending_.settings.reflectionProbes;
    return float(pending_.reflectionProbesDone + pending_.collected) /
           float(r[0] * r[1] * r[2] + std::max<std::size_t>(1, pending_.toRender.size()));
}

std::string GlobalIllumination::bakeStatus() const {
    if (!baking()) return {};
    const auto& r = pending_.settings.reflectionProbes;
    const unsigned reflections = r[0] * r[1] * r[2];
    return pending_.reflectionProbesDone < reflections
        ? "reflection probes " + std::to_string(pending_.reflectionProbesDone) + " of " + std::to_string(reflections)
        : "diffuse probes " + std::to_string(pending_.collected) + " of " + std::to_string(pending_.toRender.size());
}

void GlobalIllumination::save(const std::filesystem::path& folder) const {
    if (!active()) return;
    std::filesystem::create_directories(folder);
    lton::Writer out;
    const auto root = out.table();
    out.set(root, "format", out.string(giFormat));
    out.set(root, "version", lhat_integer(giVersion));
    const auto settings = out.table();
    writeSettings(out, settings, settings_);
    out.set(root, "settings", settings);
    const auto probes = out.table();
    for (const auto* probe : reflections_->getProbes()) {
        const auto item = out.table();
        const auto put = [&](const char* key, const Ogre::Vector3& v) { out.set(item, key, out.vector(fromOgre(v).data())); };
        put("camera", probe->getProbeCameraPos());
        put("areaCentre", probe->getArea().mCenter);
        put("areaHalfSize", probe->getArea().mHalfSize);
        put("innerRegion", probe->getAreaInnerRegion());
        put("shapeCentre", probe->getProbeShape().mCenter);
        put("shapeHalfSize", probe->getProbeShape().mHalfSize);
        out.push(probes, item);
    }
    out.set(root, "reflectionProbes", probes);
    const auto diffuse = out.table();
    out.set(diffuse, "lower", out.vector(diffuse_.lower.data()));
    out.set(diffuse, "spacing", lhat_real(diffuse_.spacing));
    const float top[3] = {float(diffuse_.top[0]), float(diffuse_.top[1]), float(diffuse_.top[2])};
    out.set(diffuse, "top", out.vector(top));
    out.set(root, "diffuse", diffuse);
    saveTexture(*reflections_->getBindTexture(), folder / "reflections.oitd");
    for (const auto& [image, file] : {std::pair{diffuse_.topNodes, "diffuse-top.oitd"}, std::pair{diffuse_.nodes, "diffuse-nodes.oitd"},
                                      std::pair{diffuse_.cells, "diffuse-cells.oitd"}, std::pair{diffuse_.probes, "diffuse-probes.oitd"}})
        image->save((folder / file).u8string(), 0, 1);
    // The irradiance field's files of an older bake.
    for (const auto* old : {"irradiance.oitd", "depth.oitd"}) std::filesystem::remove(folder / old);
    out.save(root, folder / "gi.lton");
}

GlobalIllumination::Stored GlobalIllumination::read(const std::filesystem::path& folder) {
    Stored stored;
    if (!std::filesystem::exists(folder / "gi.lton")) return stored;
    try {
        const lton::Document document(folder / "gi.lton");
        document.expect(giFormat, giVersion);
        const auto& probes = lton::table(lton::field(document.root(), "reflectionProbes"));
        for (std::size_t i = 0, count = lton::sequence(probes); i < count; ++i) {
            const auto& item = lton::table(lton::at(probes, i));
            const auto get = [&](const char* key) { Vec3 v; lton::vector(lton::field(item, key), v.data()); return v; };
            stored.probes.push_back({get("camera"), get("areaCentre"), get("areaHalfSize"), get("innerRegion"),
                                     get("shapeCentre"), get("shapeHalfSize")});
        }
        const auto& diffuse = lton::table(lton::field(document.root(), "diffuse"));
        lton::vector(lton::field(diffuse, "lower"), stored.diffuse.lower.data());
        stored.diffuse.spacing = float(lton::number(lton::field(diffuse, "spacing")));
        float top[3];
        lton::vector(lton::field(diffuse, "top"), top);
        for (int i = 0; i < 3; ++i) stored.diffuse.top[std::size_t(i)] = std::uint32_t(top[i]);
        stored.diffuse.topNodes = readTexture(folder / "diffuse-top.oitd");
        stored.diffuse.nodes = readTexture(folder / "diffuse-nodes.oitd");
        stored.diffuse.cells = readTexture(folder / "diffuse-cells.oitd");
        stored.diffuse.probes = readTexture(folder / "diffuse-probes.oitd");
        stored.reflections = readTexture(folder / "reflections.oitd");
        stored.settings = readSettings(lton::table(lton::field(document.root(), "settings")));
    } catch (const std::exception& error) {
        stored = {};
        stored.message = "Could not read baked global illumination (bake it again): " + std::string(error.what());
    }
    return stored;
}

bool GlobalIllumination::use(const Stored& stored) {
    if (!stored.message.empty()) log_(stored.message);
    if (!stored.reflections) return false;
    try {
        auto reflections = createReflections(unsigned(stored.probes.size()));
        for (const auto& placement : stored.probes) {
            auto* probe = reflections->createProbe();
            probe->setTextureParams(probeResolution, probeResolution);
            probe->initWorkspace(probeNear, probeFar);
            probe->set(toOgre(placement.camera), Ogre::Aabb(toOgre(placement.areaCentre), toOgre(placement.areaHalfSize)),
                       toOgre(placement.innerRegion), Ogre::Matrix3::IDENTITY,
                       Ogre::Aabb(toOgre(placement.shapeCentre), toOgre(placement.shapeHalfSize)));
            probe->setStatic(true);
        }
        // The probes keep the saved images; they are never rendered again.
        reflections->mPaused = true;
        uploadTexture(*stored.reflections, *reflections->getBindTexture());
        use(stored.settings, std::move(reflections), stored.diffuse);
    } catch (const std::exception& error) {
        log_("Could not use baked global illumination: " + std::string(error.what()));
        return false;
    }
    return true;
}
}
