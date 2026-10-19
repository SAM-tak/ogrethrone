#include "editor/GlobalIllumination.h"
#include "editor/Lton.h"
#include <Compositor/OgreCompositorManager2.h>
#include <Cubemaps/OgreCubemapProbe.h>
#include <Cubemaps/OgreParallaxCorrectedCubemapAuto.h>
#include <IrradianceField/OgreIrradianceField.h>
#include <OgreCamera.h>
#include <OgreDataStream.h>
#include <OgreHlmsPbs.h>
#include <OgreId.h>
#include <OgreImage2.h>
#include <OgreRoot.h>
#include <OgreTextureGpu.h>
#include <fstream>
#include <iterator>
#include <vector>

namespace throne {
namespace {
constexpr const char* giFormat = "ogrethrone-gi";
constexpr LhatInteger giVersion = 1;
/// Reflection probe cubemaps: edge in texels and format.
constexpr Ogre::uint32 probeResolution = 256;
constexpr Ogre::PixelFormatGpu probeFormat = Ogre::PFG_RGBA16_FLOAT;
constexpr float probeNear = .05f, probeFar = 500;

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

/// The settings fields a bake records, as written to and read from gi.lton.
void writeSettings(lton::Writer& out, LhatValue table, const GiSettings& s) {
    out.set(table, "lower", out.vector(s.lower.data()));
    out.set(table, "upper", out.vector(s.upper.data()));
    for (const auto& [key, counts] : {std::pair{"reflectionProbes", &s.reflectionProbes}, std::pair{"fieldProbes", &s.fieldProbes}}) {
        const float values[3] = {float((*counts)[0]), float((*counts)[1]), float((*counts)[2])};
        out.set(table, key, out.vector(values));
    }
    out.set(table, "bounces", lhat_integer(s.bounces));
}
GiSettings readSettings(const LhatTable& table) {
    GiSettings s;
    s.enabled = true;
    lton::vector(lton::field(table, "lower"), s.lower.data());
    lton::vector(lton::field(table, "upper"), s.upper.data());
    for (const auto& [key, counts] : {std::pair{"reflectionProbes", &s.reflectionProbes}, std::pair{"fieldProbes", &s.fieldProbes}}) {
        float values[3];
        lton::vector(lton::field(table, key), values);
        for (int i = 0; i < 3; ++i) (*counts)[i] = unsigned(values[i]);
    }
    s.bounces = unsigned(lton::integer(lton::field(table, "bounces")));
    return s;
}
}

GlobalIllumination::GlobalIllumination(Ogre::Root& root, Ogre::SceneManager& scene, Ogre::HlmsPbs& pbs, Log log)
    : root_(root), scene_(scene), pbs_(pbs), log_(std::move(log)) {}

GlobalIllumination::~GlobalIllumination() { clear(); }

void GlobalIllumination::clear() {
    pending_ = {};
    pbs_.setParallaxCorrectedCubemap(nullptr);
    pbs_.setIrradianceField(nullptr);
    reflections_.reset();
    field_.reset();
}

std::unique_ptr<Ogre::ParallaxCorrectedCubemapAuto> GlobalIllumination::createReflections(unsigned probes) {
    const auto* workspace = root_.getCompositorManager2()->getWorkspaceDefinition("ThroneGi/ProbeWorkspace");
    auto reflections = std::make_unique<Ogre::ParallaxCorrectedCubemapAuto>(
        Ogre::Id::generateNewId<Ogre::ParallaxCorrectedCubemapAuto>(), &root_, &scene_, workspace);
    reflections->setEnabled(true, probeResolution, probeResolution, probes, probeFormat);
    return reflections;
}

std::unique_ptr<Ogre::IrradianceField> GlobalIllumination::createField(const GiSettings& settings) {
    Ogre::IrradianceFieldSettings field;
    for (int i = 0; i < 3; ++i) field.mNumProbes[i] = settings.fieldProbes[i];
    field.mDepthProbeResolution = 16;
    field.mIrradianceResolution = 8;
    // OGRE widens the field by a cell on each side and puts probe i at grid position i
    // (the shader cannot sample past the last probe), so the region it is given puts the
    // first and last probes of each axis on the box's faces; a single probe sits centred.
    const auto span = toOgre(settings.upper - settings.lower);
    Ogre::Vector3 origin, size;
    for (int i = 0; i < 3; ++i) {
        const float n = float(settings.fieldProbes[i]);
        const float cell = n > 1 ? span[i] * n / ((n - 1) * (n + 2)) : span[i];
        size[i] = cell * n;
        origin[i] = toOgre(settings.lower)[i] + (n > 1 ? cell : span[i] * 1.5f);
    }
    field.mRasterParams.mWorkspaceName = "ThroneGi/FieldWorkspace";
    field.mRasterParams.mPixelFormat = probeFormat;
    field.mRasterParams.mCameraNear = .02f;
    field.mRasterParams.mCameraFar = float(span.length() * 2);
    field.testValidity();
    auto result = std::make_unique<Ogre::IrradianceField>(&root_, &scene_);
    result->initialize(field, origin, size, nullptr);
    return result;
}

void GlobalIllumination::use(const GiSettings& settings, std::unique_ptr<Ogre::ParallaxCorrectedCubemapAuto> reflections,
                             std::unique_ptr<Ogre::IrradianceField> field) {
    clear();
    settings_ = settings;
    reflections_ = std::move(reflections);
    field_ = std::move(field);
    pbs_.setParallaxCorrectedCubemap(reflections_.get());
    pbs_.setIrradianceField(field_.get());
}

void GlobalIllumination::beginBake(const GiSettings& settings, Ogre::Camera& camera) {
    const auto& counts = settings.reflectionProbes;
    auto reflections = createReflections(counts[0] * counts[1] * counts[2]);
    reflections->setUpdatedTrackedDataFromCamera(&camera);
    // Rendered only by continueBake(): the bake is the scene as it was then.
    reflections->mPaused = true;
    pending_ = {settings, std::move(reflections), createField(settings), 0, 0};
}

void GlobalIllumination::renderNextReflectionProbe() {
    // The probes stand on a grid over the GI box, each reaching over its cell by a quarter,
    // and are parallax-corrected against the whole box.
    const auto& settings = pending_.settings;
    const auto& counts = settings.reflectionProbes;
    const auto i = pending_.reflectionProbesDone;
    const Ogre::Vector3 cell((i % counts[0]) + .5f, ((i / counts[0]) % counts[1]) + .5f, (i / (counts[0] * counts[1])) + .5f);
    const auto lower = toOgre(settings.lower), size = toOgre(settings.upper - settings.lower);
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
    // One field update a frame: IrradianceField::update() keeps its parameters in a buffer
    // that a second call in the same frame overwrites before the GPU integrates the first
    // probes, which then keep their raw cubemap instead of irradiance. The number of probes
    // follows the time the last update took.
    const auto& f = pending_.settings.fieldProbes;
    const auto probes = (std::min)(pending_.fieldBatch, f[0] * f[1] * f[2] - pending_.fieldProbesDone);
    pending_.field->update(probes);
    pending_.fieldProbesDone += probes;
    const auto spent = std::chrono::steady_clock::now() - start;
    if (spent < budget / 2) pending_.fieldBatch *= 2;
    else if (spent > budget && pending_.fieldBatch > 1) pending_.fieldBatch /= 2;
    if (pending_.fieldProbesDone < f[0] * f[1] * f[2]) return;
    log_("Baked global illumination: " + std::to_string(pending_.reflections->getProbes().size()) + " reflection probes, " +
         std::to_string(pending_.fieldProbesDone) + " irradiance probes");
    // use() clears pending_, so the finished bake is moved out of it first.
    auto done = std::move(pending_);
    pending_ = {};
    use(done.settings, std::move(done.reflections), std::move(done.field));
}

float GlobalIllumination::bakeProgress() const {
    if (!baking()) return 1;
    const auto& r = pending_.settings.reflectionProbes;
    const auto& f = pending_.settings.fieldProbes;
    return float(pending_.reflectionProbesDone + pending_.fieldProbesDone) / float(r[0] * r[1] * r[2] + f[0] * f[1] * f[2]);
}

std::string GlobalIllumination::bakeStatus() const {
    if (!baking()) return {};
    const auto& r = pending_.settings.reflectionProbes;
    const auto& f = pending_.settings.fieldProbes;
    const unsigned reflections = r[0] * r[1] * r[2];
    return pending_.reflectionProbesDone < reflections
        ? "reflection probes " + std::to_string(pending_.reflectionProbesDone) + " of " + std::to_string(reflections)
        : "irradiance probes " + std::to_string(pending_.fieldProbesDone) + " of " + std::to_string(f[0] * f[1] * f[2]);
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
    saveTexture(*reflections_->getBindTexture(), folder / "reflections.oitd");
    saveTexture(*field_->getIrradianceTex(), folder / "irradiance.oitd");
    saveTexture(*field_->getDepthVarianceTex(), folder / "depth.oitd");
    out.save(root, folder / "gi.lton");
}

GlobalIllumination::Stored GlobalIllumination::read(const std::filesystem::path& folder, const GiSettings& settings) {
    Stored stored;
    if (!std::filesystem::exists(folder / "gi.lton")) return stored;
    try {
        const lton::Document document(folder / "gi.lton");
        document.expect(giFormat, giVersion);
        if (readSettings(lton::table(lton::field(document.root(), "settings"))) != settings) {
            stored.message = "The baked global illumination in " + folder.u8string() + " was made for other settings; bake it again";
            return stored;
        }
        const auto& probes = lton::table(lton::field(document.root(), "reflectionProbes"));
        for (std::size_t i = 0, count = lton::sequence(probes); i < count; ++i) {
            const auto& item = lton::table(lton::at(probes, i));
            const auto get = [&](const char* key) { Vec3 v; lton::vector(lton::field(item, key), v.data()); return v; };
            stored.probes.push_back({get("camera"), get("areaCentre"), get("areaHalfSize"), get("innerRegion"),
                                     get("shapeCentre"), get("shapeHalfSize")});
        }
        stored.reflections = readTexture(folder / "reflections.oitd");
        stored.irradiance = readTexture(folder / "irradiance.oitd");
        stored.depth = readTexture(folder / "depth.oitd");
        stored.settings = settings;
    } catch (const std::exception& error) {
        stored = {};
        stored.message = "Could not read baked global illumination: " + std::string(error.what());
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
        auto field = createField(stored.settings);
        uploadTexture(*stored.irradiance, *field->getIrradianceTex());
        uploadTexture(*stored.depth, *field->getDepthVarianceTex());
        use(stored.settings, std::move(reflections), std::move(field));
    } catch (const std::exception& error) {
        log_("Could not use baked global illumination: " + std::string(error.what()));
        return false;
    }
    return true;
}
}
