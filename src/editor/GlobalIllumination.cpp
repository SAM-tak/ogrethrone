#include "editor/GlobalIllumination.h"
#include "editor/Lton.h"
#include <Compositor/OgreCompositorManager2.h>
#include <Cubemaps/OgreCubemapProbe.h>
#include <Cubemaps/OgreParallaxCorrectedCubemapAuto.h>
#include <Cubemaps/OgrePccPerPixelGridPlacement.h>
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

/// Uploads @p file, as saveTexture() wrote it, into @p texture.
void loadTexture(Ogre::TextureGpu& texture, const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<char> bytes(std::istreambuf_iterator<char>(in), {});
    if (bytes.empty()) throw std::runtime_error("Could not read " + file.u8string());
    Ogre::DataStreamPtr stream(OGRE_NEW Ogre::MemoryDataStream(bytes.data(), bytes.size(), false, true));
    Ogre::Image2 image;
    image.load(stream, "oitd");
    if (image.getWidth() != texture.getWidth() || image.getPixelFormat() != texture.getPixelFormat() ||
        image.getNumMipmaps() != texture.getNumMipmaps())
        throw std::runtime_error(file.u8string() + " does not match its texture");
    image.uploadTo(&texture, 0, Ogre::uint8(image.getNumMipmaps() - 1u));
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
    const auto size = toOgre(settings.upper - settings.lower);
    field.mRasterParams.mWorkspaceName = "ThroneGi/FieldWorkspace";
    field.mRasterParams.mPixelFormat = probeFormat;
    field.mRasterParams.mCameraNear = .02f;
    field.mRasterParams.mCameraFar = float(size.length() * 2);
    field.testValidity();
    auto result = std::make_unique<Ogre::IrradianceField>(&root_, &scene_);
    result->initialize(field, toOgre(settings.lower), size, nullptr);
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

void GlobalIllumination::bake(const GiSettings& settings, Ogre::Camera& camera) {
    const auto& counts = settings.reflectionProbes;
    auto reflections = createReflections(counts[0] * counts[1] * counts[2]);
    // Places probes on a grid, renders them, then fits each one's shape to the depth it saw.
    Ogre::PccPerPixelGridPlacement grid;
    Ogre::uint32 numProbes[3] = {counts[0], counts[1], counts[2]};
    grid.setNumProbes(numProbes);
    Ogre::Aabb region;
    region.setExtents(toOgre(settings.lower), toOgre(settings.upper));
    grid.setFullRegion(region);
    grid.setOverlap(Ogre::Vector3(1.25f));
    grid.setParallaxCorrectedCubemapAuto(reflections.get());
    grid.buildStart(probeResolution, &camera, probeFormat, probeNear, probeFar);
    grid.buildEnd();
    for (auto* probe : reflections->getProbes()) probe->setStatic(true);
    // Rendered once: the bake is the scene as it was now.
    reflections->mPaused = true;
    auto field = createField(settings);
    const auto& f = settings.fieldProbes;
    field->update(f[0] * f[1] * f[2]);
    use(settings, std::move(reflections), std::move(field));
    log_("Baked global illumination: " + std::to_string(reflections_->getProbes().size()) + " reflection probes, " +
         std::to_string(f[0] * f[1] * f[2]) + " irradiance probes");
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

bool GlobalIllumination::load(const std::filesystem::path& folder, const GiSettings& settings) {
    clear();
    if (!std::filesystem::exists(folder / "gi.lton")) return false;
    try {
        const lton::Document document(folder / "gi.lton");
        document.expect(giFormat, giVersion);
        if (readSettings(lton::table(lton::field(document.root(), "settings"))) != settings) {
            log_("The baked global illumination in " + folder.u8string() + " was made for other settings; bake it again");
            return false;
        }
        const auto& probes = lton::table(lton::field(document.root(), "reflectionProbes"));
        const auto count = lton::sequence(probes);
        auto reflections = createReflections(unsigned(count));
        for (std::size_t i = 0; i < count; ++i) {
            const auto& item = lton::table(lton::at(probes, i));
            const auto get = [&](const char* key) { Vec3 v; lton::vector(lton::field(item, key), v.data()); return toOgre(v); };
            auto* probe = reflections->createProbe();
            probe->setTextureParams(probeResolution, probeResolution);
            probe->initWorkspace(probeNear, probeFar);
            probe->set(get("camera"), Ogre::Aabb(get("areaCentre"), get("areaHalfSize")), get("innerRegion"),
                       Ogre::Matrix3::IDENTITY, Ogre::Aabb(get("shapeCentre"), get("shapeHalfSize")));
            probe->setStatic(true);
        }
        // The probes keep the saved images; they are never rendered again.
        reflections->mPaused = true;
        loadTexture(*reflections->getBindTexture(), folder / "reflections.oitd");
        auto field = createField(settings);
        loadTexture(*field->getIrradianceTex(), folder / "irradiance.oitd");
        loadTexture(*field->getDepthVarianceTex(), folder / "depth.oitd");
        use(settings, std::move(reflections), std::move(field));
    } catch (const std::exception& error) {
        log_("Could not read baked global illumination: " + std::string(error.what()));
        return false;
    }
    return true;
}
}
