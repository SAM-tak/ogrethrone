#include "editor/Editor.h"
#include "editor/OgreConversions.h"
#include "editor/Lton.h"
#include "script/ScriptRuntime.h"
#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorNodeDef.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorWorkspaceDef.h>
#include <Compositor/OgreCompositorShadowNode.h>
#include <OgreLight.h>
#include <Animation/OgreBone.h>
#include <Animation/OgreSkeletonInstance.h>
#include <OgreItem.h>
#include <ozz/animation/runtime/skeleton.h>
#include <OgreForwardPlusBase.h>
#include <OgreTextureFilters.h>
#include <OgreTextureGpu.h>
#include <OgreAtmosphereNpr.h>
#include <OgreMaterialManager.h>
#include <OgreDepthBuffer.h>
#include <OgreTextureBox.h>
#include <OgreMaterial.h>
#include <OgreTechnique.h>
#include <OgrePass.h>
#include <OgreResourceGroupManager.h>
#include <OgreHlmsPbs.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreHlmsSamplerblock.h>
#include <Compositor/Pass/OgreCompositorPassDef.h>
#include <Compositor/Pass/PassDepthCopy/OgreCompositorPassDepthCopyDef.h>
#include <Compositor/Pass/PassIblSpecular/OgreCompositorPassIblSpecularDef.h>
#include <Compositor/Pass/PassQuad/OgreCompositorPassQuadDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <OgreCamera.h>
#include <OgreControllerManager.h>
#include <ParticleSystem/OgreParticleSystemManager2.h>
#include <OgreCompositorPassImguiProvider.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsUnlit.h>
#include <OgreHlmsUnlitDatablock.h>
#include <OgreImguiManager.h>
#include <OgreImage2.h>
#include <OgreLogManager.h>
#include <OgreManualObject2.h>
#include <OgreRectangle2D2.h>
#include <OgreRay.h>
#include <OgreRenderSystem.h>
#include <Vao/OgreVaoManager.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreTextureGpuManager.h>
#include <OgreWindow.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <filesystem>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace throne {
namespace {
constexpr const char* viewportWorkspaceName = "ThroneEditor/Viewport";
constexpr const char* windowWorkspaceName = "ThroneEditor/Window";
constexpr const char* lineMaterial = "ThroneEditor/Lines";
constexpr std::size_t historyLimit = 100, logLimit = 200;
/// Execution masks of the viewport's passes: every pass runs with 0x01, then either the SMAA
/// passes or the plain copy to the viewport, and the subsurface scattering passes when enabled.
constexpr Ogre::uint8 smaaPasses = 0x02, plainPasses = 0x04, subsurfacePasses = 0x08;
/// Lit opaque geometry is in queue 10 (OGRE's default), the only queue the depth prepass draws;
/// then the sky (11), unlit lines and emitters (12, which HlmsUnlit cannot draw in a prepass),
/// particles (15), absorbing volumes (99) and blended surfaces (200).
constexpr Ogre::uint8 opaqueRenderQueue = 10, skyRenderQueue = 11, unlitRenderQueue = 12;

/// ImGui's dark style with its blue accents turned to OGRE's green (#81d742).
/// Green looks brighter than blue at the same value, so the accents are dimmed.
void styleColours() {
    ImGui::StyleColorsDark();
    for (auto& colour : ImGui::GetStyle().Colors) {
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(colour.x, colour.y, colour.z, h, s, v);
        if (s < 0.2f || h < 0.5f || h > 0.75f) continue;
        ImGui::ColorConvertHSVtoRGB(94.0f / 360.0f, s, v * 0.75f, colour.x, colour.y, colour.z);
    }
}

/// Returns the registered HlmsUnlit.
/// @throws std::runtime_error if it has not been registered.
Ogre::HlmsUnlit& unlitHlms(Ogre::Root& root) {
    auto* hlms = root.getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
    if (!hlms) throw std::runtime_error("Editor requires HlmsUnlit");
    return *static_cast<Ogre::HlmsUnlit*>(hlms);
}

/// Returns the registered physically based material implementation.
Ogre::HlmsPbs& pbsHlms(Ogre::Root& root) {
    auto* hlms=root.getHlmsManager()->getHlms(Ogre::HLMS_PBS);
    if(!hlms) throw std::runtime_error("Editor requires HlmsPbs");
    return *static_cast<Ogre::HlmsPbs*>(hlms);
}

/// Creates a depth atlas with three cascades for one directional light and four
/// 1024² maps for the nearest shadow-casting point or spot lights; and, for baking
/// diffuse probes quickly (GiSettings::fastBakeShadows), the same with one 2048² map
/// for the sun instead of its cascades. Every shadow-casting light type needs a map: one
/// left without would make PBS read a shadow atlas nothing has drawn into.
void createShadowNode(Ogre::Root& root) {
    const auto create=[&](const char* name,Ogre::ShadowNodeHelper::ShadowParam sun,Ogre::uint32 localsAt) {
        Ogre::ShadowNodeHelper::ShadowParamVec maps;
        sun.addLightType(Ogre::Light::LT_DIRECTIONAL);
        maps.push_back(sun);
        for(Ogre::uint32 i=0;i<4;++i) {
            Ogre::ShadowNodeHelper::ShadowParam local{};
            local.technique=Ogre::SHADOWMAP_FOCUSED;
            local.addLightType(Ogre::Light::LT_POINT);
            local.addLightType(Ogre::Light::LT_SPOTLIGHT);
            local.resolution[0]=Ogre::ShadowNodeHelper::Resolution(1024,1024);
            local.atlasStart[0]=Ogre::ShadowNodeHelper::Resolution(i*1024,localsAt);
            maps.push_back(local);
        }
        Ogre::ShadowNodeHelper::createShadowNodeWithSettings(root.getCompositorManager2(),
            root.getRenderSystem()->getCapabilities(),name,maps,false,1024,0.7f,1.0f,0.125f,0.2f,2);
    };
    Ogre::ShadowNodeHelper::ShadowParam cascades{};
    cascades.technique=Ogre::SHADOWMAP_PSSM;
    cascades.numPssmSplits=3;
    cascades.resolution[0]=Ogre::ShadowNodeHelper::Resolution(2048,2048);
    cascades.resolution[1]=cascades.resolution[2]=Ogre::ShadowNodeHelper::Resolution(1024,1024);
    cascades.atlasStart[0]=Ogre::ShadowNodeHelper::Resolution(0,0);
    cascades.atlasStart[1]=Ogre::ShadowNodeHelper::Resolution(0,2048);
    cascades.atlasStart[2]=Ogre::ShadowNodeHelper::Resolution(1024,2048);
    create("ThroneEditor/SunShadows",cascades,3072);
    Ogre::ShadowNodeHelper::ShadowParam single{};
    single.technique=Ogre::SHADOWMAP_FOCUSED;
    single.resolution[0]=Ogre::ShadowNodeHelper::Resolution(2048,2048);
    single.atlasStart[0]=Ogre::ShadowNodeHelper::Resolution(0,0);
    create("ThroneGi/FastShadows",single,2048);
}

/// The colour of a black body at @p kelvin, scaled so 6500 K is white (Tanner Helland's
/// fit of the CIE data). Zero, used for no temperature, answers white.
Vec3 blackBody(float kelvin) {
    if(kelvin<=0) return {1,1,1};
    const auto rgb=[](double k) {
        const double t=std::clamp(k,1000.0,40000.0)/100;
        const double r=t<=66 ? 255 : 329.698727446*std::pow(t-60,-0.1332047592);
        const double g=t<=66 ? 99.4708025861*std::log(t)-161.1195681661 : 288.1221695283*std::pow(t-60,-0.0755148492);
        const double b=t>=66 ? 255 : t<=19 ? 0 : 138.5177312231*std::log(t-10)-305.0447927307;
        return Vec3{float(std::clamp(r,0.0,255.0)),float(std::clamp(g,0.0,255.0)),float(std::clamp(b,0.0,255.0))};
    };
    const auto colour=rgb(kelvin), white=rgb(6500);
    return {colour.x/white.x,colour.y/white.y,colour.z/white.z};
}

/// Creates an opaque dielectric material. Plane backfaces are lit consistently too.
Ogre::HlmsPbsDatablock* createPbsMaterial(Ogre::HlmsPbs& hlms,const std::string& name) {
    auto* material=static_cast<Ogre::HlmsPbsDatablock*>(hlms.createDatablock(name,name,
        Ogre::HlmsMacroblock(),Ogre::HlmsBlendblock(),Ogre::HlmsParamVec()));
    material->setRoughness(0.65f);
    material->setFresnel(Ogre::Vector3(0.04f),false);
    material->setTwoSidedLighting(true);
    Ogre::HlmsSamplerblock sampler;
    sampler.mU=sampler.mV=sampler.mW=Ogre::TAM_WRAP;
    for(auto slot:{Ogre::PBSM_DIFFUSE,Ogre::PBSM_NORMAL,Ogre::PBSM_ROUGHNESS,Ogre::PBSM_SPECULAR})
        material->setSamplerblock(slot,sampler);
    return material;
}

/// Converts editor Euler angles (degrees, about X, then Y, then Z) to an OGRE orientation.
Ogre::Quaternion orientation(Vec3 degrees) {
    const auto about = [](float angle, const Ogre::Vector3& axis) {
        return Ogre::Quaternion(Ogre::Degree(angle), axis);
    };
    return about(degrees.z, Ogre::Vector3::UNIT_Z) * about(degrees.y, Ogre::Vector3::UNIT_Y) *
           about(degrees.x, Ogre::Vector3::UNIT_X);
}

/// Creates a double-sided unlit material whose colour multiplies the vertex colours.
Ogre::HlmsUnlitDatablock* createMaterial(Ogre::HlmsUnlit& hlms, const std::string& name) {
    Ogre::HlmsMacroblock macroblock;
    macroblock.mCullMode = Ogre::CULL_NONE;
    auto* material = static_cast<Ogre::HlmsUnlitDatablock*>(
        hlms.createDatablock(name, name, macroblock, Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
    material->setUseColour(true);
    return material;
}

/// Uploads built-in triangle geometry to a manual object, returning local bounds.
PrimitiveMesh addPrimitiveGeometry(Ogre::ManualObject& mesh, const std::string& material, const PrimitiveShape& shape, float uvScale = 1) {
    auto data=primitiveMesh(shape);
    mesh.clear();
    if (!data.vertices.empty()) {
        mesh.begin(material, Ogre::OT_TRIANGLE_LIST);
        for (const auto& vertex:data.vertices) {
            mesh.position(toOgre(vertex.position));
            mesh.normal(toOgre(vertex.normal));
            mesh.tangent(toOgre(vertex.tangent));
            mesh.textureCoord(vertex.u*uvScale,vertex.v*uvScale);
        }
        for(auto index:data.indices) mesh.index(index);
        mesh.end();
    }
    return data;
}

/// Adds a line list from consecutive point pairs.
void addLines(Ogre::ManualObject& mesh, const std::vector<Ogre::Vector3>& points,
              const Ogre::ColourValue& colour) {
    mesh.begin(lineMaterial, Ogre::OT_LINE_LIST);
    for (Ogre::uint32 i = 0; i < points.size(); ++i) {
        mesh.position(points[i]);
        mesh.colour(colour);
        mesh.index(i);
    }
    mesh.end();
}

/// Returns a 20 x 20 unit grid on the XZ plane.
std::vector<Ogre::Vector3> gridLines() {
    std::vector<Ogre::Vector3> points;
    for (int i = -10; i <= 10; ++i) {
        const auto f = static_cast<Ogre::Real>(i);
        points.insert(points.end(), {{f, 0, -10}, {f, 0, 10}, {-10, 0, f}, {10, 0, f}});
    }
    return points;
}

/// Returns the 12 edges of a unit cube centred on the origin.
std::vector<Ogre::Vector3> cubeEdges() {
    std::vector<Ogre::Vector3> points;
    for (int axis = 0; axis < 3; ++axis) {
        for (float a : {-0.5f, 0.5f}) {
            for (float b : {-0.5f, 0.5f}) {
                Ogre::Vector3 from, to;
                from[axis] = -0.5f;
                to[axis] = 0.5f;
                from[(axis + 1) % 3] = to[(axis + 1) % 3] = a;
                from[(axis + 2) % 3] = to[(axis + 2) % 3] = b;
                points.insert(points.end(), {from, to});
            }
        }
    }
    return points;
}

/// Creates a scene node holding @p mesh under the root node.
Ogre::SceneNode* attach(Ogre::SceneManager& scene, Ogre::ManualObject* mesh) {
    auto* node = scene.getRootSceneNode()->createChildSceneNode();
    node->attachObject(mesh);
    return node;
}

/// Copies an OGRE matrix (row-major, column vectors) into ImGuizmo's column-major array.
void toColumnMajor(const Ogre::Matrix4& matrix, float* out) {
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) out[column * 4 + row] = static_cast<float>(matrix[row][column]);
}

/// The inverse of toColumnMajor.
Ogre::Matrix4 fromColumnMajor(const float* in) {
    Ogre::Matrix4 matrix;
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) matrix[row][column] = in[column * 4 + row];
    return matrix;
}

/// Level a project opens when it names no main level.
constexpr const char* defaultLevelPath = "levels/main.level.lton";

/// Where the launcher suggests creating projects: Documents/OgreThrone.
std::filesystem::path defaultProjectParent() {
#ifdef _WIN32
    if (const wchar_t* profile = _wgetenv(L"USERPROFILE"))
        return std::filesystem::path(profile) / "Documents" / "OgreThrone";
#else
    if (const char* home = std::getenv("HOME")) return std::filesystem::path(home) / "OgreThrone";
#endif
    return std::filesystem::current_path();
}

/// Formats UNIX time @p seconds as local "YYYY-MM-DD HH:MM".
std::string formatTime(std::int64_t seconds) {
    const auto time = static_cast<std::time_t>(seconds);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &local);
    return text;
}

/// Fills the window below the menu bar with a dock space. When there is no saved
/// layout that knows the current panels, it docks Scene above Systems on the left,
/// Inspector on the right, Browser and Console below and Viewport in the middle.
void dockSpace() {
    const auto* viewport = ImGui::GetMainViewport();
    const ImGuiID id = ImGui::DockSpaceOverViewport(0, viewport);
    if (const auto* node = ImGui::DockBuilderGetNode(id);
        node && node->IsSplitNode() && ImGui::FindWindowSettingsByID(ImHashStr("Systems"))) {
        // Console came later: in a layout saved before it, it joins Browser's tabs.
        if (!ImGui::FindWindowSettingsByID(ImHashStr("Console")))
            if (const auto* browser = ImGui::FindWindowSettingsByID(ImHashStr("Browser")); browser && browser->DockId)
                ImGui::DockBuilderDockWindow("Console", browser->DockId);
        // The hierarchy panel was "Scene" before it took the name of what it shows.
        if (!ImGui::FindWindowSettingsByID(ImHashStr("###Hierarchy")))
            if (const auto* scene = ImGui::FindWindowSettingsByID(ImHashStr("Scene")); scene && scene->DockId)
                ImGui::DockBuilderDockWindow("###Hierarchy", scene->DockId);
        return;
    }
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, viewport->WorkSize);
    ImGuiID centre = id;
    const ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.18f, nullptr, &centre);
    const ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.26f, nullptr, &centre);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.27f, nullptr, &centre);
    ImGuiID systems = 0;
    const ImGuiID scene = ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.55f, nullptr, &systems);
    ImGui::DockBuilderDockWindow("###Hierarchy", scene);
    ImGui::DockBuilderDockWindow("Systems", systems);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Browser", bottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Viewport", centre);
    ImGui::DockBuilderFinish(id);
}
}

Editor::Editor(Ogre::Root& root, Ogre::Window& window, Ogre::SceneManager& scene, Ogre::Camera& camera,
               const std::filesystem::path& project, std::filesystem::path projectList, AudioSystem* audio)
    : root_(root), window_(window), scene_(scene), camera_(camera), unlit_(unlitHlms(root)), pbs_(pbsHlms(root)),
      platform_(window), input_(window), gui_(std::make_unique<Ogre::ImguiManager>()),
      guiPass_(std::make_unique<Ogre::CompositorPassImguiProvider>(gui_.get())),
      projects_(projectList.empty() ? ProjectList() : ProjectList::load(projectList)),
      projectListFile_(std::move(projectList)) {
    audioDevice_=audio;
    preferencesFile_=projectListFile_.empty()?std::filesystem::path{}:projectListFile_.parent_path()/"editor.lton";
    preferences_=EditorPreferences::load(preferencesFile_);
    auto& io = ImGui::GetIO();
    // The dock layout is kept beside the project list; without one it is not persisted.
    iniFile_ = projectListFile_.empty() ? std::string() : (projectListFile_.parent_path() / "editor.ini").u8string();
    io.IniFilename = iniFile_.empty() ? nullptr : iniFile_.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    styleColours();
    // Include every BMP glyph supplied by the Japanese font, including uncommon name kanji.
    static constexpr ImWchar glyphRanges[] = {0x20, 0xFFFD, 0};
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 1;
    if (!io.Fonts->AddFontFromFileTTF(THRONE_MEDIA_DIR "/fonts/NotoSansJP-Regular.otf", 16.0f,
                                    &fontConfig, glyphRanges))
        throw std::runtime_error("Could not load the editor's Noto Sans JP font.");
    // The synchronous upload path asserts upstream (AlwaysKeepSystemRamCopy without
    // a RAM copy), so upload asynchronously and wait for it instead.
    gui_->setupFont(io.Fonts, false);
    root.getRenderSystem()->getTextureGpuManager()->waitForStreamingCompletion();

    // The provider must be installed before the dear_imgui pass is defined.
    auto* compositor = root.getCompositorManager2();
    compositor->setCompositorPassProvider(guiPass_.get());
    scene_.getParticleSystemManager2()->setHighestPossibleQuota(16383u,0u);
    pbs_.setShadowSettings(Ogre::HlmsPbs::PCF_3x3);
    scene.setForwardClustered(true,16,8,24,64,0,0,0.1f,100.0f);
    scene.setShadowFarDistance(100.0f);
    scene.setAmbientLight(Ogre::ColourValue(.22f,.25f,.30f),Ogre::ColourValue(.06f,.07f,.09f),Ogre::Vector3::UNIT_Y);
    sun_=scene.createLight();
    sunNode_=scene.getRootSceneNode()->createChildSceneNode();
    sunNode_->attachObject(sun_);
    sun_->setType(Ogre::Light::LT_DIRECTIONAL);
    sun_->setDirection(Ogre::Vector3(-.6f,-1.0f,-.4f).normalisedCopy());
    sun_->setPowerScale(Ogre::Math::PI);
    sun_->setCastShadows(true);
    // Load only the standard sky programs/material; unrelated sample materials stay out.
    auto& resources = Ogre::ResourceGroupManager::getSingleton();
    const Ogre::String skyGroup = "ThroneEditorSky";
    const Ogre::String common = THRONE_OGRE_MEDIA_DIR "/2.0/scripts/materials/Common";
    resources.createResourceGroup(skyGroup);
    for (const auto* folder : {"", "/GLSL", "/Any"})
        resources.addResourceLocation(common + folder, "FileSystem", skyGroup);
    resources.addResourceLocation(THRONE_MEDIA_DIR "/postprocess", "FileSystem", skyGroup);
    // Reuse OGRE's SMAA implementation and lookup tables, with a fixed High preset.
    const Ogre::String smaa = THRONE_OGRE_MEDIA_DIR "/2.0/scripts/materials/Tutorial_SMAA";
    resources.addResourceLocation(smaa, "FileSystem", skyGroup);
    resources.addResourceLocation(smaa + "/Vulkan", "FileSystem", skyGroup);
    // The compute job that prefilters environment cubemaps for rough reflections.
    // ...and the workspaces that bake global illumination.
    for (const auto* folder : {"/Compute/Algorithms/IBL", "/Compute/Tools/Any", "/Hlms/Common/Any", "/Hlms/Common/GLSL"})
        resources.addResourceLocation(THRONE_OGRE_MEDIA_DIR + Ogre::String(folder), "FileSystem", skyGroup);
    resources.addResourceLocation(THRONE_MEDIA_DIR "/gi", "FileSystem", skyGroup);
    root.getHlmsManager()->loadMaterials("IBL.material.json", skyGroup, nullptr, "");
    for (const auto* script : {"Quad.program", "Atmosphere.material", "Sky.material", "DPSM.material", "Copyback.material",
                               "PccDepthCompressor.material", "Tonemap.material", "Bloom.material", "Taa.material", "MotionBlur.material", "Ssao.material", "Ssr.material", "DepthOfField.material", "Subsurface.material", "ThroneSmaa.material",
                               "ThroneGi.compositor"}) {
        auto stream = resources.openResource(script, skyGroup);
        Ogre::MaterialManager::getSingleton().parseScript(stream, skyGroup);
    }
    // LTC requires these tables independently of whether a level currently uses LTC.
    pbs_.loadLtcMatrix();
    pbs_.setAreaLightMasks(root.getRenderSystem()->getTextureGpuManager()->reservePoolId(759384u,256u,256u,8u,9u,Ogre::PFG_R8_UNORM));
    sky_ = std::make_unique<Ogre::AtmosphereNpr>(root.getRenderSystem()->getVaoManager());
    auto preset = sky_->getPreset();
    preset.fogDensity = 0;
    // Keep the horizon restrained in this LDR editor viewport, including downward views.
    preset.horizonLimit = 0.15f;
    preset.skyPower = 0.5f;
    sky_->setPreset(preset);
    sky_->setSky(&scene_, true);
    drawSkyAfterOpaque();
    models_ = std::make_unique<ModelLibrary>(root, scene, pbs_, [this](const std::string& line) { log(line); });
    pbsListeners_.add(&frameNoise_);
    pbsListeners_.add(&motionVectors_);
    ambientOcclusion_ = std::make_unique<AmbientOcclusion>(*root.getHlmsManager());
    pbsListeners_.add(ambientOcclusion_.get());
    pbs_.setListener(&pbsListeners_);
    gi_ = std::make_unique<GlobalIllumination>(root, scene, pbs_, pbsListeners_, [this](const std::string& line) { log(line); });
    createShadowNode(root);
    tonemap_ = Ogre::MaterialManager::getSingleton().getByName("Throne/Tonemap", skyGroup);
    taa_ = Ogre::MaterialManager::getSingleton().getByName("Throne/Taa", skyGroup);
    motionBlur_ = Ogre::MaterialManager::getSingleton().getByName("Throne/MotionBlur", skyGroup);
    ssao_ = Ogre::MaterialManager::getSingleton().getByName("Throne/Ssao", skyGroup);
    ssaoBlur_ = Ogre::MaterialManager::getSingleton().getByName("Throne/SsaoBlur", skyGroup);
    ssr_ = Ogre::MaterialManager::getSingleton().getByName("Throne/Ssr", skyGroup);
    depthOfField_ = Ogre::MaterialManager::getSingleton().getByName("Throne/DepthOfField", skyGroup);
    subsurfaceHorizontal_ = Ogre::MaterialManager::getSingleton().getByName("Throne/SubsurfaceHorizontal", skyGroup);
    subsurfaceVertical_ = Ogre::MaterialManager::getSingleton().getByName("Throne/SubsurfaceVertical", skyGroup);
    {
        // A depth prepass draws lit opaque geometry (queue 10) into Depth and a G-buffer of
        // view-space normals, sun shadow/roughness, motion vectors (MotionVectors, the
        // change in UV since the previous frame) and subsurface widths, from which SSAO computes the ambient
        // occlusion (Ssao) and SSR the reflections (Ssr, from the previous frame's TaaHistory)
        // that queue 10 then reads. The scene then renders in half float:
        // queue 10 into Opaque reusing the prepass (depth read-only), whose surfaces with
        // subsurface scattering are blurred, then the other queues below 200. Opaque is copied to Hdr; refractive glass (queue 200 and up) samples
        // Opaque and its depth while drawing into Hdr. Depth of field (with an f-stop) defocuses
        // Hdr into Focused; temporal anti-aliasing blends that into the reprojected history (Taa,
        // copied to TaaHistory for the next frame), smoothing the defocus's grain too, and writes
        // each pixel's Velocity, along which motion blur (in Play) smears Taa into Blurred. Bloom
        // is combined before tone mapping, then SMAA filters the LDR scene before ImGui presents
        // it (the UI stays sharp).
        auto* node = compositor->addNodeDefinition("ThroneEditor/ViewportNode");
        node->addTextureSourceName("Viewport", 0, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
        node->setNumLocalTextureDefinitions(24);
        for (const auto* name : {"VelocityTiles", "VelocityNeighbours"}) {
            // One texel per 16x16 pixels (see media/postprocess/MotionTiles).
            auto* texture = node->addTextureDefinition(name);
            texture->format = Ogre::PFG_RG16_FLOAT;
            texture->widthFactor = texture->heightFactor = 1.0f / 16;
            texture->depthBufferId = Ogre::DepthBuffer::POOL_NO_DEPTH;
        }
        for (const auto* name : {"Ssr", "SubsurfaceHorizontal"}) {
            auto* texture = node->addTextureDefinition(name);
            texture->format = Ogre::PFG_RGBA16_FLOAT;
            texture->depthBufferId = Ogre::DepthBuffer::POOL_NO_DEPTH;
        }
        for (const auto* name : {"SsaoRaw", "Ssao"}) {
            auto* texture = node->addTextureDefinition(name);
            texture->format = Ogre::PFG_R8_UNORM;
            texture->depthBufferId = Ogre::DepthBuffer::POOL_NO_DEPTH;
        }
        node->addTextureDefinition("Depth")->format = Ogre::PFG_D32_FLOAT;
        node->addTextureDefinition("GBufferNormals")->format = Ogre::PFG_R10G10B10A2_UNORM;
        node->addTextureDefinition("GBufferShadowRoughness")->format = Ogre::PFG_RG16_UNORM;
        node->addTextureDefinition("GBufferMotion")->format = Ogre::PFG_RG16_FLOAT;
        node->addTextureDefinition("GBufferSubsurface")->format = Ogre::PFG_R16_FLOAT;
        {
            auto* view = node->addRenderTextureView("GBuffer");
            for (const auto* name : {"GBufferNormals", "GBufferShadowRoughness", "GBufferMotion", "GBufferSubsurface"}) {
                Ogre::RenderTargetViewEntry attachment;
                attachment.textureName = name;
                view->colourAttachments.push_back(attachment);
            }
            view->depthAttachment.textureName = "Depth";
        }
        for (const auto* name : {"Opaque", "Hdr"}) {
            node->addTextureDefinition(name)->format = Ogre::PFG_RGBA16_FLOAT;
            auto* view = node->addRenderTextureView(name);
            Ogre::RenderTargetViewEntry attachment;
            attachment.textureName = name;
            view->colourAttachments.push_back(attachment);
            view->depthAttachment.textureName = "Depth";
        }
        for (const auto* name : {"Velocity", "Focused", "Blurred", "Taa", "TaaHistory"}) {
            auto* texture = node->addTextureDefinition(name);
            texture->format = Ogre::String(name) == "Velocity" ? Ogre::PFG_RG16_FLOAT : Ogre::PFG_RGBA16_FLOAT;
            texture->depthBufferId = Ogre::DepthBuffer::POOL_NO_DEPTH;
        }
        // The history must survive until the next frame.
        node->getLocalTextureDefinitionsNonConst().back().textureFlags &= ~Ogre::TextureFlags::DiscardableContent;
        for (const auto* name : {"BloomBright", "BloomHorizontal", "Bloom"}) {
            auto* texture = node->addTextureDefinition(name);
            texture->format = Ogre::PFG_RGBA16_FLOAT;
            texture->widthFactor = texture->heightFactor = 0.25f;
            texture->depthBufferId = Ogre::DepthBuffer::POOL_NO_DEPTH;
        }
        for (const auto* name : {"Tonemapped", "SmaaEdges", "SmaaWeights"}) {
            auto* texture = node->addTextureDefinition(name);
            texture->format = Ogre::String(name) == "SmaaEdges" ? Ogre::PFG_RG8_UNORM : Ogre::PFG_RGBA8_UNORM;
            texture->depthBufferId = Ogre::DepthBuffer::POOL_NO_DEPTH;
        }
        node->setNumTargetPass(23);
        // @p outputs are further colour targets the material writes after @p target.
        const auto quad = [&](const char* target, const char* material, std::initializer_list<const char*> inputs,
                              std::initializer_list<const char*> outputs = {}) {
            if (Ogre::String(target) != "Viewport") {
                auto* view = node->addRenderTextureView(target);
                const auto attach = [&](const char* name) {
                    Ogre::RenderTargetViewEntry attachment;
                    attachment.textureName = name;
                    view->colourAttachments.push_back(attachment);
                };
                attach(target);
                for (const auto* name : outputs) attach(name);
                view->depthBufferId = Ogre::DepthBuffer::POOL_NO_DEPTH;
            }
            auto* pass = static_cast<Ogre::CompositorPassQuadDef*>(node->addTargetPass(target)->addPass(Ogre::PASS_QUAD));
            pass->mMaterialName = material;
            Ogre::uint32 unit = 0;
            for (const auto* input : inputs) pass->addQuadTextureSource(unit++, input);
            pass->setAllLoadActions(Ogre::LoadAction::DontCare);
            pass->mStoreActionDepth = pass->mStoreActionStencil = Ogre::StoreAction::DontCare;
            return pass;
        };
        auto* prepass = static_cast<Ogre::CompositorPassSceneDef*>(node->addTargetPass("GBuffer")->addPass(Ogre::PASS_SCENE));
        prepass->mShadowNode = "ThroneEditor/SunShadows";
        prepass->mPrePassMode = Ogre::PrePassCreate;
        // mLastRQ is exclusive.
        prepass->mLastRQ = opaqueRenderQueue + 1;
        prepass->setAllLoadActions(Ogre::LoadAction::Clear);
        prepass->setAllClearColours(Ogre::ColourValue(0.5f, 0.5f, 1.0f, 1.0f));
        // Where nothing is drawn (the sky), motion comes from the camera alone; its users
        // reproject the depth for it.
        prepass->mClearColour[2] = Ogre::ColourValue(0, 0, 0, 0);
        // Nor any subsurface scattering.
        prepass->mClearColour[3] = Ogre::ColourValue(0, 0, 0, 0);
        prepass->mStoreActionStencil = Ogre::StoreAction::DontCare;
        quad("SsaoRaw", "Throne/Ssao", {"GBufferNormals", "Depth"});
        quad("Ssao", "Throne/SsaoBlur", {"SsaoRaw", "Depth"});
        quad("Ssr", "Throne/Ssr", {"GBufferNormals", "GBufferShadowRoughness", "Depth", "GBufferMotion", "TaaHistory"});
        auto* opaque = static_cast<Ogre::CompositorPassSceneDef*>(node->addTargetPass("Opaque")->addPass(Ogre::PASS_SCENE));
        opaque->mShadowNode = "ThroneEditor/SunShadows";
        opaque->mShadowNodeRecalculation = Ogre::SHADOW_NODE_REUSE;
        opaque->setAllClearColours(Ogre::ColourValue(0.11f, 0.12f, 0.14f));
        opaque->setAllLoadActions(Ogre::LoadAction::Load);
        opaque->mLoadActionColour[0] = Ogre::LoadAction::Clear;
        opaque->mStoreActionStencil = Ogre::StoreAction::DontCare;
        opaque->mLastRQ = opaqueRenderQueue + 1;
        Ogre::IdStringVec gbuffer;
        gbuffer.push_back("GBufferNormals");
        gbuffer.push_back("GBufferShadowRoughness");
        // HlmsPbs reserves a texture slot for the prepass depth; leaving it unnamed shifts the
        // later textures (the BRDF lookup with them), so the read-only depth is bound there too.
        // HlmsPbs blends Ssr's reflections over the reflection probes by their alpha.
        opaque->setUseDepthPrePass(gbuffer, "Depth", "Ssr");
        // AmbientOcclusion binds it; the barrier solver must make it readable.
        opaque->mExposedTextures.push_back("Ssao");
        // Subsurface scattering blurs the lit surfaces that have it (GBufferSubsurface) across,
        // into a copy, then down, back over them, before anything else is drawn in front.
        quad("SubsurfaceHorizontal", "Throne/SubsurfaceHorizontal", {"Opaque", "GBufferSubsurface", "Depth"})->mExecutionMask =
            subsurfacePasses;
        {
            auto* view = node->addRenderTextureView("OpaqueColour");
            Ogre::RenderTargetViewEntry attachment;
            attachment.textureName = "Opaque";
            view->colourAttachments.push_back(attachment);
            view->depthBufferId = Ogre::DepthBuffer::POOL_NO_DEPTH;
            auto* pass = static_cast<Ogre::CompositorPassQuadDef*>(node->addTargetPass("OpaqueColour")->addPass(Ogre::PASS_QUAD));
            pass->mMaterialName = "Throne/SubsurfaceVertical";
            Ogre::uint32 unit = 0;
            for (const auto* input : {"SubsurfaceHorizontal", "GBufferSubsurface", "Depth"}) pass->addQuadTextureSource(unit++, input);
            // It discards the pixels it leaves alone.
            pass->setAllLoadActions(Ogre::LoadAction::Load);
            pass->mExecutionMask = subsurfacePasses;
        }
        auto* rest = static_cast<Ogre::CompositorPassSceneDef*>(node->addTargetPass("Opaque")->addPass(Ogre::PASS_SCENE));
        rest->mShadowNode = "ThroneEditor/SunShadows";
        rest->mShadowNodeRecalculation = Ogre::SHADOW_NODE_REUSE;
        rest->setAllLoadActions(Ogre::LoadAction::Load);
        rest->mStoreActionStencil = Ogre::StoreAction::DontCare;
        rest->mFirstRQ = opaqueRenderQueue + 1;
        rest->mLastRQ = 200;
        auto* hdr = node->addTargetPass("Hdr");
        static_cast<Ogre::CompositorPassDepthCopyDef*>(hdr->addPass(Ogre::PASS_DEPTHCOPY))->setDepthTextureCopy("Opaque", "Hdr");
        auto* refractive = static_cast<Ogre::CompositorPassSceneDef*>(hdr->addPass(Ogre::PASS_SCENE));
        refractive->mShadowNode = "ThroneEditor/SunShadows";
        refractive->mShadowNodeRecalculation = Ogre::SHADOW_NODE_REUSE;
        refractive->setAllLoadActions(Ogre::LoadAction::Load);
        refractive->mStoreActionDepth = refractive->mStoreActionStencil = Ogre::StoreAction::DontCare;
        refractive->mFirstRQ = 200;
        refractive->setUseRefractions("Depth", "Opaque");
        quad("Focused", "Throne/DepthOfField", {"Hdr", "Depth"});
        quad("Taa", "Throne/Taa", {"Focused", "TaaHistory", "GBufferMotion", "Depth"}, {"Velocity"});
        quad("TaaHistory", "Ogre/Copy/4xFP32", {"Taa"});
        quad("VelocityTiles", "Throne/MotionTileMax", {"Velocity"});
        quad("VelocityNeighbours", "Throne/MotionNeighbourMax", {"VelocityTiles"});
        quad("Blurred", "Throne/MotionBlur", {"Taa", "Velocity", "Depth", "VelocityNeighbours"});
        quad("BloomBright", "Throne/BloomExtract", {"Blurred"});
        quad("BloomHorizontal", "Throne/BloomHorizontal", {"BloomBright"});
        quad("Bloom", "Throne/BloomVertical", {"BloomHorizontal"});
        quad("Tonemapped", "Throne/Tonemap", {"Blurred", "Bloom"});
        // Edge detection discards pixels without edges, so clear them every frame.
        // Run weights over the whole target instead of allocating a stencil buffer.
        // SMAA runs when the view's anti-aliasing asks for it; otherwise the tone-mapped picture
        // is copied (see the execution masks in jitterCamera).
        auto* edges = quad("SmaaEdges", "Throne/SmaaEdgeDetection", {"Tonemapped"});
        edges->mLoadActionColour[0] = Ogre::LoadAction::Clear;
        edges->setAllClearColours(Ogre::ColourValue(0, 0, 0, 0));
        edges->mExecutionMask = smaaPasses;
        quad("SmaaWeights", "Throne/SmaaBlendingWeightCalculation", {"SmaaEdges"})->mExecutionMask = smaaPasses;
        quad("Viewport", "Throne/SmaaNeighborhoodBlending", {"Tonemapped", "SmaaWeights"})->mExecutionMask = smaaPasses;
        quad("Viewport", "Ogre/Copy/4xFP32", {"Tonemapped"})->mExecutionMask = plainPasses;
        compositor->addWorkspaceDefinition(viewportWorkspaceName)->connectExternal(0, node->getName(), 0);
    }
    {
        // Integrates a radiance cubemap's GGX lobes into the mipmaps of a second cubemap.
        auto* node = compositor->addNodeDefinition("ThroneEditor/IblNode");
        node->addTextureSourceName("Radiance", 0, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
        node->addTextureSourceName("Prefiltered", 1, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
        node->setNumTargetPass(1);
        auto* pass = static_cast<Ogre::CompositorPassIblSpecularDef*>(node->addTargetPass("Prefiltered")->addPass(Ogre::PASS_IBL_SPECULAR));
        pass->setCubemapInput("Radiance");
        pass->setCubemapOutput("Prefiltered");
        pass->mAutogenInputMipmaps = false;
        auto* workspace = compositor->addWorkspaceDefinition("ThroneEditor/Ibl");
        workspace->connectExternal(0, node->getName(), 0);
        workspace->connectExternal(1, node->getName(), 1);
    }
    auto* node = compositor->addNodeDefinition("ThroneEditor/WindowNode");
    node->addTextureSourceName("Window", 0, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
    node->addTextureSourceName("Viewport", 1, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
    node->setNumTargetPass(1);
    auto* target = node->addTargetPass("Window");
    target->setNumPasses(2);
    // The ImGui pass never makes its own render pass current, so its PSO would be
    // built for the previous target (the viewport). A clear pass makes the window current.
    auto* clear = target->addPass(Ogre::PASS_CLEAR);
    clear->setAllClearColours(Ogre::ColourValue(0.04f, 0.06f, 0.10f));
    auto* pass = target->addPass(Ogre::PASS_CUSTOM, "dear_imgui");
    for (auto* each : {clear, pass})
        each->mStoreActionDepth = each->mStoreActionStencil = Ogre::StoreAction::DontCare;
    pass->mLoadActionDepth = pass->mLoadActionStencil = Ogre::LoadAction::DontCare;
    // Lets the barrier solver transition the viewport texture for sampling by ImGui.
    pass->mExposedTextures.push_back("Viewport");
    auto* workspace = compositor->addWorkspaceDefinition(windowWorkspaceName);
    workspace->connectExternal(0, node->getName(), 0);
    workspace->connectExternal(1, node->getName(), 1);

    texture_ = root.getRenderSystem()->getTextureGpuManager()->createTexture(
        "ThroneEditor/ViewportTexture", Ogre::GpuPageOutStrategy::Discard,
        Ogre::TextureFlags::RenderToTexture, Ogre::TextureTypes::Type2D);
    // Not sRGB: tone mapping writes sRGB-encoded values, which ImGui copies unchanged to the
    // (non-sRGB) window. An sRGB view would be decoded to linear when sampled and shown dark.
    texture_->setPixelFormat(Ogre::PFG_RGBA8_UNORM);
    resizeViewport(640, 360);

    createMaterial(unlit_, lineMaterial)->setColour(Ogre::ColourValue::White);
    grid_ = scene.createManualObject();
    grid_->setCastShadows(false);
    grid_->setRenderQueueGroup(unlitRenderQueue);
    addLines(*grid_, gridLines(), Ogre::ColourValue(0.35f, 0.37f, 0.40f));
    gridNode_ = attach(scene, grid_);
    outline_ = scene.createManualObject();
    outline_->setCastShadows(false);
    outline_->setRenderQueueGroup(unlitRenderQueue);
    addLines(*outline_, cubeEdges(), Ogre::ColourValue(1.0f, 0.75f, 0.2f));
    outlineNode_ = attach(scene, outline_);
    giBoxes_ = scene.createManualObject();
    giBoxes_->setCastShadows(false);
    giBoxes_->setRenderQueueGroup(unlitRenderQueue);
    giBoxesNode_ = attach(scene, giBoxes_);
    camera.setAutoAspectRatio(false);
    rebuildScene();
    updateCamera();

    std::snprintf(newParent_, sizeof newParent_, "%s", defaultProjectParent().u8string().c_str());
    projectsChanged();
    if (!project.empty()) openProject(project);
}

void Editor::openProject(const std::filesystem::path& path) {
    try {
        auto project = Project::open(path);
        if (runtime_) stop();
        // The console's own session belongs to the project it was made for.
        consoleIdle_.reset();
        consoleMore_ = nullptr;
        project_ = std::move(project);
        projects_.touch(project_->root);
        projectsChanged();
        writeHostApi();
        const auto level = project_->mainLevel.empty() ? std::string(defaultLevelPath) : project_->mainLevel;
        std::snprintf(levelPath_, sizeof levelPath_, "%s", level.c_str());
        library_ = Library(project_->root);
        doc_ = Document();
        doc_.gameRendering=project_->rendering;
        contexts_.resize(1);
        navigate_.reset();
        if (std::filesystem::exists(project_->resolve(level)) && !open()) throw std::runtime_error(fileError_);
        undo_.clear();
        redo_.clear();
        dirty_ = false;
        refreshView();
        launcherError_.clear();
        log("Opened project " + project_->name + " (" + project_->root.u8string() + ")");
    } catch (const std::exception& error) {
        project_.reset();
        launcherError_ = error.what();
    }
}

void Editor::closeProject() {
    if (runtime_) stop();
    consoleIdle_.reset();
    consoleMore_ = nullptr;
    project_.reset();
    doc_ = Document();
    contexts_.resize(1);
    navigate_.reset();
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    selected_ = {};
    refreshView();
    projectsChanged();
}

void Editor::writeHostApi() {
    const auto json = ScriptRuntime::hostApi();
    const auto path = project_->resolve("lhat-host.json");
    std::ifstream current(path, std::ios::binary);
    if (current && std::string((std::istreambuf_iterator<char>(current)), {}) == json) return;
    current.close();
    try {
        lton::writeAtomically(path, json);
        log("Updated lhat-host.json");
    } catch (const std::exception& error) {
        log(std::string("Could not write lhat-host.json: ") + error.what());
    }
}

void Editor::projectsChanged() {
    if (!projectListFile_.empty()) {
        try {
            projects_.save(projectListFile_);
        } catch (const std::exception& error) {
            launcherError_ = std::string("Could not save the project list: ") + error.what();
        }
    }
    known_.clear();
    for (const auto& entry : projects_.entries) {
        Known known{entry.root, entry.root.filename().u8string(), formatTime(entry.lastOpened), false};
        try {
            known.name = Project::open(entry.root).name;
            known.found = true;
        } catch (const std::exception&) {
        }
        known_.push_back(std::move(known));
    }
}

void Editor::launcher() {
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("Launcher", nullptr, flags)) {
        const auto folderInput = [&](const char* label, const char* hint, char* text, std::size_t capacity) {
            ImGui::PushID(label);
            ImGui::InputTextWithHint(label, hint, text, capacity);
            ImGui::SameLine();
            if (ImGui::Button("Browse...")) {
                try {
                    if (const auto folder = platform_.pickFolder(std::filesystem::u8path(text))) {
                        const auto path = folder->u8string();
                        if (path.size() >= capacity) throw std::runtime_error("The selected folder path is too long.");
                        std::copy(path.begin(), path.end(), text);
                        text[path.size()] = '\0';
                        launcherError_.clear();
                    }
                } catch (const std::exception& error) {
                    launcherError_ = error.what();
                }
            }
            ImGui::PopID();
        };
        ImGui::TextUnformatted("Ogre Throne - Projects");
        ImGui::Separator();
        if (ImGui::Button("New Project")) form_ = Form::New, launcherError_.clear();
        ImGui::SameLine();
        if (ImGui::Button("Import")) form_ = Form::Import, launcherError_.clear();
        if (form_ == Form::New) {
            ImGui::InputText("Name", newName_, sizeof newName_);
            folderInput("Parent folder", "parent folder for the new project", newParent_, sizeof newParent_);
            if (ImGui::Button("Create")) {
                try {
                    openProject(Project::create(std::filesystem::u8path(newParent_), newName_).root);
                    if (project_) form_ = Form::None;
                } catch (const std::exception& error) {
                    launcherError_ = error.what();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) form_ = Form::None;
        } else if (form_ == Form::Import) {
            folderInput("Folder", "folder containing project.throne.lton", importPath_, sizeof importPath_);
            if (ImGui::Button("Import and Open")) {
                openProject(std::filesystem::u8path(importPath_));
                if (project_) form_ = Form::None;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) form_ = Form::None;
        }
        if (!launcherError_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", launcherError_.c_str());
        ImGui::Separator();

        // Acting while iterating would invalidate known_, so remember the choice.
        std::optional<std::filesystem::path> open, forget;
        if (known_.empty()) ImGui::TextDisabled("No projects yet. Create one or import an existing folder.");
        if (ImGui::BeginTable("Projects", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1);
            ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch, 2);
            ImGui::TableSetupColumn("Last opened", ImGuiTableColumnFlags_WidthFixed, 130);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < known_.size(); ++i) {
                const auto& known = known_[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!known.found);
                if (ImGui::Selectable(known.name.c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    open = known.root;
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(known.root.u8string().c_str());
                if (!known.found) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "(missing)");
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(known.lastOpened.c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Remove")) forget = known.root;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (open) openProject(*open);
        if (forget) {
            projects_.remove(*forget);
            projectsChanged();
        }
    }
    ImGui::End();
}

void Editor::play() {
    paused_ = false;
    // Play always runs the level (TDDja/06 6), on its expansion only.
    contexts_.resize(1);
    navigate_.reset();
    selected_ = {};
    refreshView();
    try {
        // Play runs the open level; playing the whole game (with main.lh) comes later.
        runtime_ = std::make_unique<ScriptRuntime>(
            doc_.world, view_, project_->root, [this](const std::string& line) { log(line); }, PlayScope::Level);
        // Scripts read the poses the Animation stage samples, on the same thread.
        runtime_->setJointQuery([this](std::size_t instance, std::size_t node, std::string_view joint, float matrix[16]) {
            return animation_.jointMatrix(instance, node, joint, matrix);
        });
        if(audioDevice_) {
            const auto& path=doc_.world.level.mixer;
            const auto mixer=path.empty()?MixerDefinition{}:
                (doc_.mixers.count(path)?doc_.mixers.at(path):MixerDefinition::load(project_->resolve(path)));
            audioWorld_=std::make_unique<AudioWorld>(*audioDevice_,project_->root,mixer,[this](const std::string& line){log(line);});
            runtime_->setAudioCallbacks([this](std::size_t i,std::size_t n,AudioCommand command){audioWorld_->command(i,n,command);},
                [this](std::size_t i,std::size_t n){return audioWorld_->playing(i,n);});
        }
        physics_=std::make_unique<PhysicsWorld>();
        physics_->setSerial(doc_.world.level.serialExecution);
        physics_->load(view_);
        renderSnapshot_ = view_;
        renderEvents_ = std::make_unique<ScriptRuntime>(
            doc_.world, *renderSnapshot_, project_->root, [this](const std::string& line) { log(line); },
            PlayScope::RenderEvents);
        clearParticles();
        log("Play");
    } catch (const std::exception& error) {
        renderEvents_.reset();
        runtime_.reset();
        physics_.reset();
        audioWorld_.reset();
        log(std::string("Play refused:\n") + error.what());
    }
}

void Editor::stop() {
    paused_ = false;
    if (consoleMore_ == runtime_.get()) consoleMore_ = nullptr;
    renderEvents_.reset();
    runtime_.reset();
    audioWorld_.reset();
    physics_.reset();
    renderSnapshot_.reset();
    clearParticles();
    log("Stop");
}

void Editor::togglePause() {
    if (!runtime_) return;
    paused_ = !paused_;
    if(audioWorld_) audioWorld_->setPaused(paused_);
    log(paused_ ? "Pause" : "Resume");
}

Editor::~Editor() {
    auto& shutdownLog=Ogre::LogManager::getSingleton();
    shutdownLog.logMessage("Shutdown: releasing script runtimes");
    consoleMore_=nullptr;
    consoleIdle_.reset();
    renderEvents_.reset();
    runtime_.reset();
    audioWorld_.reset();
    shutdownLog.logMessage("Shutdown: releasing physics world and workers");
    physics_.reset();
    renderSnapshot_.reset();
    shutdownLog.logMessage("Shutdown: releasing editor rendering resources");
    // ImGui would save to io.IniFilename when its context is destroyed, after iniFile_ is gone.
    auto& io = ImGui::GetIO();
    if (io.IniFilename) ImGui::SaveIniSettingsToDisk(io.IniFilename);
    io.IniFilename = nullptr;
    destroyWorkspaces();
    // Baked probes use workspaces and compute jobs from the resource group destroyed below.
    gi_.reset();
    pbs_.setListener(nullptr);
    // The cubemap sky's material belongs to the resource group destroyed below.
    scene_.setSky(false, Ogre::SceneManager::SkyCubemap, nullptr);
    tonemap_.reset();
    taa_.reset();
    motionBlur_.reset();
    ssao_.reset();
    ssaoBlur_.reset();
    ssr_.reset();
    depthOfField_.reset();
    subsurfaceHorizontal_.reset();
    subsurfaceVertical_.reset();
    sky_.reset();
    Ogre::ResourceGroupManager::getSingleton().destroyResourceGroup("ThroneEditorSky");
    root_.getCompositorManager2()->setCompositorPassProvider(nullptr);
    // ImGui materials reference the viewport texture, so release them first.
    guiPass_.reset();
    gui_.reset();
    root_.getRenderSystem()->getTextureGpuManager()->destroyTexture(texture_);
    destroyRenderObjects();
    models_.reset();
    for (auto* mesh : {grid_, outline_, giBoxes_}) scene_.destroyManualObject(mesh);
    for (auto* node : {gridNode_, outlineNode_, giBoxesNode_}) scene_.destroySceneNode(node);
    unlit_.destroyDatablock(lineMaterial);
    scene_.destroyLight(sun_);
    scene_.destroySceneNode(sunNode_);
    shutdownLog.logMessage("Shutdown: editor released");
}

bool Editor::frame(float deltaSeconds, const std::function<void()>& input) {
    using Clock = std::chrono::steady_clock;
    const auto frameStart = Clock::now();
    const auto milliseconds = [](auto begin, auto end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    FrameTimings measured;
    platform_.newFrame();
    input_.capture();
    if (input) input();
    if (platform_.takeCloseRequest()) requestLeave(Leave::Exit);
    if (close_) return false;
    ImGui::GetIO().DeltaTime = std::max(deltaSeconds, 1.0e-4f);
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    viewportVisible_=false;
    if (!project_) {
        launcher();
        applyDisplaySettings();
        gui_->prepareForRender(&scene_);
        return root_.renderOneFrame();
    }
    // Drill-down steps change between frames, so no panel sees view_ change under it.
    if (navigate_) {
        contexts_ = std::move(*navigate_);
        navigate_.reset();
        selected_ = {};
    }
    refreshView();
    shortcuts();
    menu();
    dockSpace();
    scenePanel();
    systemsPanel();
    inspector();
    viewport();
    browser();
    mixerPanel();
    console();
    fileDialog();
    tasks_.draw();
    closeDialog();
    replaceDialog();
    applyDisplaySettings();
    if(close_) {
        ImGui::EndFrame();
        return false;
    }
    measured.uiMs = milliseconds(frameStart, Clock::now());
    bool running = true;
    if (physics_) physics_->setSerial(doc_.world.level.serialExecution);
    // Read OGRE only on the main thread, after navigation/Inspector edits. Workers
    // receive a value snapshot; even INT_MAX scene priorities cannot override it.
    std::optional<AudioWorld::ListenerPose> sceneViewListener;
    if(viewAudioListener_ && viewportVisible_) {
        updateCamera();
        const auto rotation=camera_.getDerivedOrientation();
        sceneViewListener=AudioWorld::ListenerPose{fromOgre(camera_.getDerivedPosition()),
            fromOgre(rotation*Ogre::Vector3::UNIT_X),fromOgre(rotation*Ogre::Vector3::UNIT_Y),
            fromOgre(rotation*Ogre::Vector3::NEGATIVE_UNIT_Z)};
    }
    // Simulation stages may run on the worker thread: they touch view_, physics_ and their
    // own timing fields only. The main thread reads view_ only at the Sync barriers.
    const auto engine = [&,sceneViewListener](const std::string& stage) {
        const auto start = Clock::now();
        if (stage == "Box3DStep") {
            if (physics_) physics_->step(view_);
            measured.physicsMs += milliseconds(start, Clock::now());
        } else if (stage == "SteamAudio") {
            if(audioWorld_) audioWorld_->spatialStep(view_,sceneViewListener);
        } else if (stage == "SoLoud") {
            if(audioWorld_) audioWorld_->step(view_);
        } else if (stage == "Animation") {
            if (runtime_) animation_.step(view_, float(fixedStep));
        } else if (stage == "SyncSimulation") {
            if (runtime_) renderSnapshot_ = view_, animation_.publish();
            measured.snapshotMs = milliseconds(start, Clock::now());
        } else if (stage == renderStages[0] || stage == renderStages[2]) {
            if (renderEvents_) renderEvents_->renderEvent(stage, deltaSeconds);
        } else if (stage == "OGRE") {
            if(viewportVisible_) {
                // While editing, the poses follow the Animators' times as edited.
                if (!runtime_) animation_.step(view_, 0), animation_.publish();
                syncTransforms(runtime_ && renderSnapshot_ ? &*renderSnapshot_ : nullptr);
                updateCamera();
                scene_.getParticleSystemManager2()->setCameraPosition(camera_.getDerivedPosition());
                jitterCamera();
            } else taaReset_ = true, historyReady_ = false;
            gui_->prepareForRender(&scene_);
            // Scene RGB is already composited. Particle alpha must not blend the viewport again.
            // The pinned ImGui backend creates this per-texture material during prepareForRender.
            if(auto material=Ogre::MaterialManager::getSingleton().getByName(
                   "!!OgreImgui_"+texture_->getName().getReleaseText())) {
                auto* pass=material->getTechnique(0)->getPass(0);
                if(pass->getBlendblock()->mSourceBlendFactor!=Ogre::SBF_ONE ||
                   pass->getBlendblock()->mDestBlendFactor!=Ogre::SBF_ZERO)
                    pass->setBlendblock(Ogre::HlmsBlendblock());
            }
            measured.renderSyncMs = milliseconds(start, Clock::now());
            const auto render = Clock::now();
            Ogre::ControllerManager::getSingleton().setTimeFactor(paused()?0.0f:1.0f);
            running = root_.renderOneFrame(deltaSeconds);
            // Picking and the gizmo see the camera without jitter.
            camera_.setFrustumOffset(0, 0);
            measured.renderMs = milliseconds(render, Clock::now());
        }
        // FetchInput has already collected this frame's OS/ImGui/OIS input above.
    };
    try {
        if(audioWorld_) {
            const auto& path=doc_.world.level.mixer;
            if(path.empty()) audioWorld_->setMixer(MixerDefinition{});
            else if(doc_.mixers.count(path)) audioWorld_->setMixer(doc_.mixers.at(path));
        }
        FrameTimes times;
        if (paused()) engine("OGRE");
        else if (runtime_) times = runtime_->update(deltaSeconds, engine);
        else times = runFrame(ScriptRuntime::schedule(doc_.world, view_), doc_.world.level.serialExecution, 0, engine);
        measured.scriptMs = (std::max)(0.0, times.simulationMs - measured.physicsMs);
        measured.waitMs = times.waitMs;
    } catch (const std::runtime_error& error) {
        log(error.what());
        stop();
        syncTransforms();
        gui_->prepareForRender(&scene_);
        running = root_.renderOneFrame(deltaSeconds);
    }
    measured.frameMs = milliseconds(frameStart, Clock::now());
    measured.nodes = renderObjects_.size();
    measured.bodies = physics_ ? physics_->bodyCount() : 0;
    measured.particleSystems=particleSystemCount_; measured.particles=particleCount_;
    timings_ = measured;
    const auto weight = averageTimings_.frameMs == 0 ? 1.0 : 1.0 - std::exp(-measured.frameMs / 500.0);
    for (auto member : {&FrameTimings::frameMs, &FrameTimings::uiMs, &FrameTimings::scriptMs,
         &FrameTimings::snapshotMs, &FrameTimings::physicsMs, &FrameTimings::renderSyncMs,
         &FrameTimings::renderMs, &FrameTimings::waitMs})
        averageTimings_.*member += weight * (measured.*member - averageTimings_.*member);
    return running;
}

void Editor::setEditorVSync(bool enabled) {
    if(preferences_.vsync==enabled) return;
    preferences_.vsync=enabled;
    try { preferences_.save(preferencesFile_); }
    catch(const std::exception& error) { log(std::string("Could not save editor preferences: ")+error.what()); }
}

bool Editor::sceneViewVisible() const { return viewportWorkspace_ && viewportWorkspace_->getEnabled(); }

void Editor::applyDisplaySettings() {
    const bool sceneVisible=project_.has_value() && viewportVisible_;
    const bool vsync=!sceneVisible || preferences_.vsync;
    if(window_.getVSync()!=vsync) {
        // setVSync recreates Vulkan's swapchain; finish its outstanding GPU work first.
        auto* vao=root_.getRenderSystem()->getVaoManager();
        vao->waitForSpecificFrameToFinish(vao->getFrameCount());
        window_.setVSync(vsync,1);
    }
    if(viewportWorkspace_) viewportWorkspace_->setEnabled(sceneVisible);
}

void Editor::setShadowsEnabled(bool enabled) {
    shadowsEnabled_=enabled;
    sun_->setCastShadows(enabled);
    for(auto& render:renderObjects_) if(render.light)
        render.light->setCastShadows(enabled && render.light->getType()==Ogre::Light::LT_DIRECTIONAL);
}


namespace {
/// The radical inverse of @p index in @p base: a low-discrepancy sequence in [0, 1).
float halton(unsigned index, unsigned base) {
    float result = 0, fraction = 1;
    for (; index; index /= base) result += (fraction /= float(base)) * float(index % base);
    return result;
}
}

void Editor::jitterCamera() {
    // The clip space the motion vectors use: HlmsPbs flips the projection as the targets need.
    const bool flip = viewportWorkspace_->getNodeSequence()[0]->getDefinedTexture("GBufferMotion")->requiresTextureFlipping();
    const auto clip = [&] {
        auto projection = camera_.getProjectionMatrixWithRSDepth();
        if (flip)
            for (int column = 0; column < 4; ++column) projection[1][column] = -projection[1][column];
        return projection;
    };
    const auto steady = clip();
    // Eight sub-pixel offsets of the Halton (2, 3) sequence, applied as an off-axis shift
    // (OGRE ignores it for orthographic views, which then get no TAA).
    // Orthographic views cannot be jittered (OGRE ignores the offset): they get SMAA instead.
    const auto mode = forcedAntiAliasing_.value_or(viewAntiAliasing_);
    const bool on = mode == AntiAliasing::Temporal && !orthographic_;
    const bool smaa = mode == AntiAliasing::Smaa || (mode == AntiAliasing::Temporal && orthographic_);
    viewportWorkspace_->setExecutionMask(0x01 | (smaa ? smaaPasses : plainPasses) | (subsurfaceEnabled_ ? subsurfacePasses : 0));
    if (on) {
        taaIndex_ = taaIndex_ % 8 + 1;
        const float x = (halton(taaIndex_, 2) - 0.5f) * 2 / float(viewportWidth_);
        const float y = (halton(taaIndex_, 3) - 0.5f) * 2 / float(viewportHeight_);
        const float tanY = std::tan(camera_.getFOVy().valueRadians() * 0.5f) * camera_.getFocalLength();
        camera_.setFrustumOffset(x * tanY * camera_.getAspectRatio(), y * tanY);
    }
    const auto jittered = clip();
    const auto centre = [](const Ogre::Matrix4& projection) {
        const auto p = projection * Ogre::Vector4(0, 0, -1, 1);
        return Ogre::Vector2(p.x / p.w, p.y / p.w);
    };
    const auto jitter = (centre(jittered) - centre(steady)) * 0.5f;
    const auto viewProj = jittered * camera_.getViewMatrix(true);
    auto params = taa_->getTechnique(0)->getPass(0)->getFragmentProgramParameters();
    params->setNamedConstant("reproject", taaViewProj_ * viewProj.inverse());
    params->setNamedConstant("jitter", jitter);
    params->setNamedConstant("jitterDelta", jitter - taaJitter_);
    params->setNamedConstant("currentWeight", on && !taaReset_ ? 0.1f : 1.0f);
    params->setNamedConstant("reverseDepth", root_.getRenderSystem()->isReverseDepth() ? 1.0f : 0.0f);
    // Motion blur over a 180 degree shutter while the game runs; the distance to a depth d
    // is B / (d + A) with the projection's z row (A, B).
    auto blur = motionBlur_->getTechnique(0)->getPass(0)->getFragmentProgramParameters();
    blur->setNamedConstant("shutter", motionBlurInPlay_ && runtime_ && !paused() ? 0.5f : 0.0f);
    blur->setNamedConstant("depthParams", Ogre::Vector2(float(steady[2][2]), float(steady[2][3])));
    // Ambient occlusion over half a metre, from the depth as this frame draws it.
    auto ssao = ssao_->getTechnique(0)->getPass(0)->getFragmentProgramParameters();
    ssao->setNamedConstant("projection", jittered);
    ssao->setNamedConstant("inverseProjection", jittered.inverse());
    ssao->setNamedConstant("radius", 0.5f);
    ssao->setNamedConstant("strength", ambientOcclusionEnabled_ ? 1.0f : 0.0f);
    ssao->setNamedConstant("reverseDepth", root_.getRenderSystem()->isReverseDepth() ? 1.0f : 0.0f);
    ssaoBlur_->getTechnique(0)->getPass(0)->getFragmentProgramParameters()->setNamedConstant(
        "depthParams", Ogre::Vector2(float(steady[2][2]), float(steady[2][3])));
    // Reflections read the previous frame, so none while it shows nothing usable.
    auto ssr = ssr_->getTechnique(0)->getPass(0)->getFragmentProgramParameters();
    ssr->setNamedConstant("projection", jittered);
    ssr->setNamedConstant("inverseProjection", jittered.inverse());
    ssr->setNamedConstant("jitterDelta", jitter - taaJitter_);
    ssr->setNamedConstant("strength", reflectionsEnabled_ && historyReady_ ? 1.0f : 0.0f);
    ssr->setNamedConstant("reverseDepth", root_.getRenderSystem()->isReverseDepth() ? 1.0f : 0.0f);
    historyReady_ = true;
    // With temporal anti-aliasing, the per-pixel noise of the screen-space passes (and the
    // alpha-hash threshold, through FrameNoise) moves on every
    // frame (an additive recurrence of the golden ratio), so that it averages away; without, it
    // stays put rather than flicker.
    if (on) taaNoise_ = std::fmod(taaNoise_ + 0.618034f, 1.0f);
    frameNoise_.value = taaNoise_;
    for (auto* material : {&ssao_, &ssr_, &depthOfField_, &subsurfaceHorizontal_, &subsurfaceVertical_})
        (*material)->getTechnique(0)->getPass(0)->getFragmentProgramParameters()->setNamedConstant("frameNoise", taaNoise_);
    // Subsurface widths become pixels at the surface's distance.
    for (auto* material : {&subsurfaceHorizontal_, &subsurfaceVertical_}) {
        auto params = (*material)->getTechnique(0)->getPass(0)->getFragmentProgramParameters();
        params->setNamedConstant("pixelScale", Ogre::Vector2(std::abs(float(steady[0][0])) * 0.5f * float(viewportWidth_),
                                                             std::abs(float(steady[1][1])) * 0.5f * float(viewportHeight_)));
        params->setNamedConstant("depthParams", Ogre::Vector2(float(steady[2][2]), float(steady[2][3])));
    }
    // A thin lens on a 35 mm full-frame sensor (24 mm high) of this field of view: the circle
    // of confusion's diameter in pixels is A f / (zf - f) * (z - zf) / z over the sensor's height.
    auto dof = depthOfField_->getTechnique(0)->getPass(0)->getFragmentProgramParameters();
    const float focal = 12.0f / std::tan(camera_.getFOVy().valueRadians() * 0.5f), focus = viewFocusDistance_ * 1000.0f;
    const bool defocus = viewFStop_ > 0 && !orthographic_ && focus > focal;
    dof->setNamedConstant("focusDistance", viewFocusDistance_);
    dof->setNamedConstant("cocScale", defocus ? focal / viewFStop_ * focal / (focus - focal) / 24.0f * float(viewportHeight_) : 0.0f);
    dof->setNamedConstant("depthParams", Ogre::Vector2(float(steady[2][2]), float(steady[2][3])));
    taaViewProj_ = viewProj;
    taaJitter_ = jitter;
    taaReset_ = !on;
}

void Editor::setSkyboxEnabled(bool enabled) {
    skyboxEnabled_ = enabled;
    // An environment replaces the atmosphere as the sky.
    auto* cubemap = environment_ ? environment_->cubemap : nullptr;
    sky_->setSky(&scene_, enabled && !cubemap);
    scene_.setSky(enabled && cubemap, Ogre::SceneManager::SkyCubemap, cubemap);
    drawSkyAfterOpaque();
}

void Editor::drawSkyAfterOpaque() {
    // OGRE puts both skies in render queue 212, after the blended queue (200), so the sky
    // painted over transparent surfaces and was missing from what refraction samples.
    // Draw it right after opaque geometry (queue 10) and before particles (15) instead.
    // SceneManager::setSky makes a new rectangle each time it is enabled.
    auto* root = scene_.getRootSceneNode(Ogre::SCENE_STATIC);
    for (size_t i = 0; i < root->numAttachedObjects(); ++i)
        if (auto* object = root->getAttachedObject(i);
            object->getRenderQueueGroup() == 212u && dynamic_cast<Ogre::Rectangle2D*>(object))
            object->setRenderQueueGroup(skyRenderQueue);
}

void Editor::captureViewport(const std::filesystem::path& path) {
    Ogre::Image2 image;
    image.convertFromTexture(texture_,0,0);
    // The viewport holds sRGB-encoded RGBA8 bytes, the colours shown.
    if(image.getPixelFormat()!=Ogre::PFG_RGBA8_UNORM) throw std::runtime_error("Unexpected viewport capture format");
    const auto box=image.getData(0);
    std::ofstream stream(path,std::ios::binary);
    stream << "P6\n" << box.width << ' ' << box.height << "\n255\n";
    for(unsigned y=0;y<box.height;++y) {
        const auto* row=static_cast<const char*>(box.atFromOffsettedOrigin(0,y,0));
        for(unsigned x=0;x<box.width;++x) stream.write(row+x*4,3);
    }
    if(!stream) throw std::runtime_error("Could not save viewport capture");
}

Prefab* Editor::content() {
    const auto& context = contexts_.back();
    if (context.kind == Context::Kind::Scene) return &doc_.world.scenes[context.path];
    if (context.kind == Context::Kind::Prefab) return &doc_.prefabs[context.path];
    return nullptr;
}

void Editor::refreshView() {
    if (runtime_) return;
    // Opened prefabs show their unsaved edits wherever they are instantiated.
    for (const auto& [prefabPath, prefab] : doc_.prefabs) library_.put(prefabPath, prefab);
    if (auto* edited = content()) {
        view_ = {};
        view_.instances.push_back({0, expand(*edited, library_, contexts_.back().path)});
    } else {
        view_ = expand(doc_.world, library_);
    }
    references_ = findReferences(view_, content() ? nullptr : &doc_.world.level);
    decltype(viewShape_) shape;
    for (std::size_t i = 0; i < view_.instances.size(); ++i)
        for (const auto& node : view_.instances[i].expansion.nodes) shape.emplace_back(i, node.path, node.parent);
    // Play can grow the rendered scene without updating the authored shape cache.
    // Restore those objects before any panel (including picking and gizmos) uses their indices.
    bool rebuild = shape != viewShape_ || sceneNodes_.size() != view_.instances.size();
    for (std::size_t i = 0; i < sceneNodes_.size() && i < view_.instances.size(); ++i)
        rebuild |= sceneNodes_[i].size() != view_.instances[i].expansion.nodes.size();
    if (rebuild) {
        viewShape_ = std::move(shape);
        rebuildScene();
    }
}

void Editor::enter(Context context) {
    if (context.kind == Context::Kind::Scene && !doc_.world.scenes.count(context.path)) {
        const auto file = project_->resolve(context.path);
        try {
            doc_.world.scenes[context.path] = std::filesystem::exists(file) ? Scene::load(file) : Scene();
        } catch (const std::exception& error) {
            log("Cannot open " + context.path + ": " + error.what());
            return;
        }
    }
    if (context.kind == Context::Kind::Prefab && !doc_.prefabs.count(context.path)) {
        try {
            doc_.prefabs[context.path] = library_.prefab(context.path);
        } catch (const std::exception& error) {
            log("Cannot open " + context.path + ": " + error.what());
            return;
        }
    }
    navigate_ = contexts_;
    navigate_->push_back(std::move(context));
}

Prefab* Editor::fileOf(int instance) {
    if (auto* edited = content()) return edited;
    if (instance < 0 || instance >= static_cast<int>(view_.instances.size())) return nullptr;
    const auto* placed = doc_.world.level.findScene(view_.instances[instance].id);
    const auto scene = placed ? doc_.world.scenes.find(placed->scene) : doc_.world.scenes.end();
    return scene == doc_.world.scenes.end() ? nullptr : &scene->second;
}

Editor::NodeTarget Editor::fileTarget(int instance, const NodePath& path) {
    NodeTarget target;
    // At the level, a node is edited in its scene's file (TDDja/06 3.1).
    auto* file = fileOf(instance);
    if (!file || (path.empty() && !file->variant())) return target;
    // A variant has no nodes of its own: every edit is one of its overrides.
    if (file->variant()) {
        target.overrides = &file->overrides;
        target.relative = path;
        return target;
    }
    auto* own = file->findNode(path[0]);
    if (!own) return target;
    if (path.size() == 1) {
        target.node = own;
        // An instance gives its root only a name and transform; the rest are overrides.
        if (!own->prefab.empty()) {
            target.overrides = &own->overrides;
            target.holder = own;
        }
    } else {
        target.overrides = &own->overrides;
        target.holder = own;
        target.relative.assign(path.begin() + 1, path.end());
    }
    return target;
}

Editor::NodeTarget Editor::targetOf(int instance, const NodePath& path) {
    auto target = fileTarget(instance, path);
    // A node an instance added is edited in place, inside that instance's overrides.
    while (target.overrides && !target.relative.empty()) {
        auto& added = target.overrides->added;
        const auto it = std::find_if(added.begin(), added.end(),
                                     [&](const AddedNode& entry) { return entry.node.id == target.relative[0]; });
        if (it == added.end()) break;
        target.relative.erase(target.relative.begin());
        if (target.relative.empty()) {
            target.addedTo = target.overrides;
            target.addedBy = target.holder;
            target.node = &it->node;
            target.overrides = it->node.prefab.empty() ? nullptr : &it->node.overrides;
        } else {
            target.overrides = &it->node.overrides;
        }
        target.holder = target.overrides ? &it->node : nullptr;
    }
    return target;
}

Editor::SystemTarget Editor::systemTarget() {
    SystemTarget target;
    if (selected_.kind != Selection::Kind::System) return target;
    auto* file = fileOf(selected_.instance);
    if (selected_.instance < 0) {
        target.system = doc_.world.level.findSystem(selected_.system);
    } else if (!file) {
        return target;
    } else if (file->variant()) {
        target.overrides = &file->overrides;
        target.instance = selected_.path;
    } else if (selected_.path.empty()) {
        target.system = file->findSystem(selected_.system);
    } else if (auto* own = file->findNode(selected_.path[0])) {
        target.overrides = &own->overrides;
        target.holder = own;
        target.instance.assign(selected_.path.begin() + 1, selected_.path.end());
    }
    // A system an instance added is edited in place, inside that instance's overrides
    // (or those of a node it added, for that node's systems).
    while (target.overrides) {
        if (target.instance.empty()) {
            auto& added = target.overrides->addedSystems;
            const auto it = std::find_if(added.begin(), added.end(), [&](const System& each) { return each.id == selected_.system; });
            if (it != added.end()) {
                target.system = &*it;
                target.addedTo = target.overrides;
                target.addedBy = target.holder;
                target.overrides = nullptr;
                target.holder = nullptr;
            }
            break;
        }
        auto& nodes = target.overrides->added;
        const auto it = std::find_if(nodes.begin(), nodes.end(),
                                     [&](const AddedNode& each) { return each.node.id == target.instance[0]; });
        if (it == nodes.end()) break;
        target.overrides = &it->node.overrides;
        target.holder = &it->node;
        target.instance.erase(target.instance.begin());
    }
    return target;
}

const ExpandedNode* Editor::selectedNode() {
    if (selected_.kind != Selection::Kind::Node || selected_.instance < 0 ||
        selected_.instance >= static_cast<int>(view_.instances.size()))
        return nullptr;
    const auto& expansion = view_.instances[selected_.instance].expansion;
    const auto at = expansion.find(selected_.path);
    return at == Expansion::npos ? nullptr : &expansion.nodes[at];
}

SceneInstance* Editor::selectedScene() {
    if (selected_.kind != Selection::Kind::Scene || content() || selected_.instance < 0 ||
        selected_.instance >= static_cast<int>(view_.instances.size()))
        return nullptr;
    return doc_.world.level.findScene(view_.instances[selected_.instance].id);
}

const System* Editor::selectedSystem() {
    if (selected_.kind != Selection::Kind::System) return nullptr;
    if (selected_.instance < 0) return doc_.world.level.findSystem(selected_.system);
    if (selected_.instance >= static_cast<int>(view_.instances.size())) return nullptr;
    for (const auto& expanded : view_.instances[selected_.instance].expansion.systems)
        if (expanded.instance == selected_.path && expanded.system.id == selected_.system) return &expanded.system;
    return nullptr;
}

void Editor::writeNode(const SceneNode& value, unsigned changed) {
    const auto target = targetOf(selected_.instance, selected_.path);
    const auto* transform = value.transform ? &*value.transform : nullptr;
    const auto* box = value.box ? &*value.box : nullptr;
    if (target.node) {
        if (changed & 1) target.node->name = value.name;
        if (transform && (changed & 14)) {
            auto& into = target.node->transform ? *target.node->transform : target.node->transform.emplace();
            if (changed & 2) into.position = transform->position;
            if (changed & 4) into.rotation = transform->rotation;
            if (changed & 8) into.scale = transform->scale;
        }
        // An instance's root takes everything but its Transform from the prefab.
        if (box && (changed & 16)) {
            if (target.overrides) target.overrides->node({}).colour = box->colour;
            else if (target.node->box) target.node->box->colour = box->colour;
        }
    } else if (target.overrides) {
        auto& entry = target.overrides->node(target.relative);
        if (changed & 1) entry.name = value.name;
        if (transform && (changed & 2)) entry.position = transform->position;
        if (transform && (changed & 4)) entry.rotation = transform->rotation;
        if (transform && (changed & 8)) entry.scale = transform->scale;
        if (box && (changed & 16)) entry.colour = box->colour;
    }
}

void Editor::rebuildScene() {
    destroyRenderObjects();
    syncTransforms();
}

void Editor::destroyRenderObjects() {
    clearParticles();
    // Children before parents, so no node is destroyed while it still has children.
    for (auto it = renderObjects_.rbegin(); it != renderObjects_.rend(); ++it) {
        if(it->light) scene_.destroyLight(it->light);
        if(it->emitter) {
            scene_.destroyManualObject(it->emitter);
            scene_.destroySceneNode(it->emitterNode);
            unlit_.destroyDatablock(it->emitterMaterial->getName());
        }
        if(it->lightNode) scene_.destroySceneNode(it->lightNode);
        models_->remove(it->placedModel);
        scene_.destroyManualObject(it->mesh);
        scene_.destroySceneNode(it->node);
        pbs_.destroyDatablock(it->material->getName());
    }
    renderObjects_.clear();
    for (auto* root : sceneRoots_) scene_.destroySceneNode(root);
    sceneRoots_.clear();
    sceneNodes_.clear();
}

std::filesystem::path Editor::mediaFile(const std::string& path) const {
    return path.rfind("@ogre/",0)==0 ? std::filesystem::path(THRONE_OGRE_MEDIA_DIR)/path.substr(6) : project_->resolve(path);
}

Ogre::TextureGpu* Editor::projectTexture(const std::string& path,int kind,bool mask) {
    if(path.empty() || !project_) return nullptr;
    const auto file=mediaFile(path);
    const auto key=std::make_tuple(file.generic_u8string(),kind,mask);
    if(const auto it=projectTextures_.find(key); it!=projectTextures_.end()) return it->second;
    if(!std::filesystem::exists(file)) {
        log("Texture not found: "+file.u8string()); projectTextures_[key]=nullptr; return nullptr;
    }
    // Keep resource names and aliases qualified to avoid collisions between projects.
    auto& resources=Ogre::ResourceGroupManager::getSingleton();
    const auto folder=file.parent_path().generic_u8string();
    const Ogre::String group="ThroneTextures/"+folder;
    if(!resources.resourceGroupExists(group)) resources.createResourceGroup(group);
    if(!resources.resourceLocationExists(folder,group)) resources.addResourceLocation(folder,"FileSystem",group);
    auto* manager=root_.getRenderSystem()->getTextureGpuManager();
    auto* texture=manager->createOrRetrieveTexture(file.filename().u8string(),file.generic_u8string()+"/"+std::to_string(kind)+(mask?"/mask":""),
        Ogre::GpuPageOutStrategy::Discard,static_cast<Ogre::CommonTextureTypes::CommonTextureTypes>(kind),group,mask?759384u:0u);
    projectTextures_[key]=texture;
    return texture;
}

void Editor::prefilter(Ogre::TextureGpu& cubemap) {
    auto* textures=root_.getRenderSystem()->getTextureGpuManager();
    auto* target=textures->createTexture("ThroneEditor/IblTarget",Ogre::GpuPageOutStrategy::Discard,
        Ogre::TextureFlags::RenderToTexture|Ogre::TextureFlags::Uav|Ogre::TextureFlags::DiscardableContent,Ogre::TextureTypes::TypeCube);
    target->copyParametersFrom(&cubemap);
    target->_setDepthBufferDefaults(Ogre::DepthBuffer::POOL_NO_DEPTH,false,Ogre::PFG_UNKNOWN);
    target->scheduleTransitionTo(Ogre::GpuResidency::Resident);
    auto* compositor=root_.getCompositorManager2();
    auto* workspace=compositor->addWorkspace(&scene_,Ogre::CompositorChannelVec{&cubemap,target},&camera_,"ThroneEditor/Ibl",false);
    workspace->_update();
    compositor->removeWorkspace(workspace);
    // Mip 0 stays the mirror reflection; each further mip holds a rougher lobe.
    for(Ogre::uint8 mip=1;mip<target->getNumMipmaps();++mip) {
        const auto box=target->getEmptyBox(mip);
        target->copyTo(&cubemap,box,mip,box,mip);
    }
    textures->destroyTexture(target);
}

Ogre::TextureGpu* Editor::environmentReflection() const {
    return environment_ && !gi_->active() ? environment_->cubemap : nullptr;
}

void Editor::requestGlobalIlluminationBake() {
    if(baking_) return;
    baking_=true;
    struct Bake {
        unsigned passes, left;
        std::filesystem::path folder;
        bool dropped=false;
        std::chrono::steady_clock::time_point start{};
    };
    // The scene whose GI is shown is baked, where its instance is placed (TDDja/13 4).
    if(giInstance()<0 || lightingScenePath().empty()) {
        baking_=false;
        log("Nothing to bake: global illumination belongs to a scene");
        return;
    }
    const unsigned passes=lighting().gi.bounces;
    auto bake=std::make_shared<Bake>(Bake{passes,passes,giFolder()});
    tasks_.add("Baking global illumination",[this,bake] {
        // The first pass sees only the environment's light: an earlier bake is dropped and the
        // lighting it replaced returns next frame. Each later pass, a frame after the one before,
        // is lit by it and adds a bounce. Passes wait for files being read and for textures.
        if(!bake->dropped) {
            bake->dropped=true;
            if(gi_->active()) { gi_->clear(); return false; }
        }
        if(!gi_->baking()) {
            if(reads_ || revealing_ || !root_.getRenderSystem()->getTextureGpuManager()->isDoneStreaming()) return false;
            // The diffuse probes are those placed for the settings and scene as they are.
            updateProbeLayout();
            if(placing_ || layoutSettings_!=lighting().gi) return false;
            // The time recorded runs from the first pass's start, after files are read.
            if(bake->left==bake->passes) bake->start=std::chrono::steady_clock::now();
            gi_->beginBake(lighting().gi,camera_,octree_,instanceTransform(giInstance()));
            return false;
        }
        // The editor's lines stay out of the probes; syncTransforms() shows them again.
        for(auto* node:{gridNode_,outlineNode_,giBoxesNode_}) node->setVisible(false);
        // Probes render for about a tenth of a second each frame.
        gi_->continueBake(std::chrono::milliseconds(100));
        if(gi_->baking() || --bake->left) return false;
        gi_->save(bake->folder,std::chrono::duration<double>(std::chrono::steady_clock::now()-bake->start).count());
        log("Global illumination saved into "+bake->folder.u8string());
        baking_=false;
        return true;
    },[this,bake] {
        return (float(bake->passes-bake->left)+(gi_->baking() ? gi_->bakeProgress() : 0.0f))/float(bake->passes);
    },[this,bake] {
        return "Pass "+std::to_string(bake->passes-bake->left+1)+" of "+std::to_string(bake->passes)+" (one bounce each)"+
            (gi_->baking() ? ": "+gi_->bakeStatus() : std::string());
    });
}

void Editor::readGlobalIllumination() {
    ++reads_;
    const auto folder=giFolder();
    tasks_.background<GlobalIllumination::Stored>("Reading global illumination",[folder] {
        return GlobalIllumination::read(folder);
    },[this,folder](GlobalIllumination::Stored& stored) {
        --reads_;
        // A bake for the scene still shown, with GI still enabled.
        if(!baking_ && giFolder()==folder && lighting().gi.enabled && giInstance()>=0)
            gi_->use(stored,instanceTransform(giInstance()));
    });
}

void Editor::revealModels() {
    if(revealing_ || !models_->hidden()) return;
    revealing_=true;
    tasks_.add("Preparing materials",[this] {
        // Two new materials a frame: each compiles its shaders the first time it is drawn.
        // It waits for files being read, as an environment or bake arriving later changes
        // every pass, which would compile all shaders shown so far again.
        if(reads_) return false;
        models_->reveal(2);
        revealing_=models_->hidden()!=0;
        return !revealing_;
    },[this] { return models_->hiddenTotal() ? 1.0f-float(models_->hidden())/float(models_->hiddenTotal()) : 1.0f; },
    [this] { return std::to_string(models_->hiddenTotal()-models_->hidden())+" of "+std::to_string(models_->hiddenTotal())+" model parts shown"; });
}

std::vector<SurfacePoint> Editor::sceneSurface(int instance) const {
    std::vector<SurfacePoint> points;
    if (instance < 0 || std::size_t(instance) >= sceneRoots_.size()) return points;
    const auto toScene = instanceTransform(instance).inverseAffine();
    for (const auto& render : renderObjects_) {
        if (!render.placedModel.root || render.instance != std::size_t(instance)) continue;
        const auto m = toScene * render.placedModel.root->_getFullTransformUpdated();
        for (const auto& p : models_->surface(render.modelKey)) {
            const auto world = m.transformAffine(toOgre(p.position));
            points.push_back({{world.x, world.y, world.z}, p.volume});
        }
    }
    return points;
}

bool Editor::sceneBounds(Vec3& lower, Vec3& upper, int instance) const {
    bool any = false;
    if (instance < 0 || std::size_t(instance) >= sceneRoots_.size()) return false;
    const auto toScene = instanceTransform(instance).inverseAffine();
    for (const auto& render : renderObjects_) {
        if (render.instance != std::size_t(instance)) continue;
        const auto m = toScene * render.node->_getFullTransformUpdated();
        for (int corner = 0; corner < 8; ++corner) {
            const Ogre::Vector3 local(corner & 1 ? render.upper.x : render.lower.x, corner & 2 ? render.upper.y : render.lower.y,
                                      corner & 4 ? render.upper.z : render.lower.z);
            const auto p = m.transformAffine(local);
            if (!any) lower = upper = {p.x, p.y, p.z};
            any = true;
            lower = {std::min(lower.x, p.x), std::min(lower.y, p.y), std::min(lower.z, p.z)};
            upper = {std::max(upper.x, p.x), std::max(upper.y, p.y), std::max(upper.z, p.z)};
        }
    }
    return any;
}

void Editor::updateProbeLayout() {
    // The probes of the scene whose GI is shown, in its coordinates (TDDja/13).
    const auto instance = giInstance();
    if (instance < 0) return;
    const auto& gi = lighting().gi;
    std::size_t placed = 0;
    for (const auto& render : renderObjects_) placed += render.placedModel.root != nullptr && render.instance == std::size_t(instance);
    // Global: the bounds of the scene's drawn nodes, as Unity's APV sizes a Global volume.
    Vec3 lower = gi.lower, upper = gi.upper;
    if (!gi.localProbeVolume && !sceneBounds(lower, upper, instance)) lower = upper = {};
    // One placement at a time, on a worker thread; settings changed meanwhile (a drag) are
    // placed once it is done.
    if (placing_ || (gi == layoutSettings_ && placed == layoutModels_ && lower == layoutLower_ && upper == layoutUpper_)) return;
    layoutSettings_ = gi;
    layoutModels_ = placed;
    layoutLower_ = lower;
    layoutUpper_ = upper;
    placing_ = true;
    tasks_.background<ProbeOctree>("Placing GI probes",
        [gi, lower, upper, surface = sceneSurface(instance)] {
            return AdaptiveIrradianceField::build(lower, upper, gi.probeSpacing, gi.probeOffset, gi.minObjectVolume,
                                                  gi.fillEmptySpaces, surface);
        },
        [this](ProbeOctree& result) {
            octree_ = std::move(result);
            ++layoutVersion_;
            placing_ = false;
        });
}

void Editor::readModel(const ModelComponent& model, const std::string& key) {
    ++reads_;
    const auto file=mediaFile(model.path);
    // The worker gets material changes' maps as files.
    auto resolved=model;
    for(auto& m:resolved.materials) if(!m.diffuseMap.empty()) m.diffuseMap=mediaFile(m.diffuseMap).u8string();
    tasks_.background<ModelLibrary::Parsed>("Reading "+model.path,[file,resolved] { return ModelLibrary::parse(file,resolved); },
        [this,key,path=model.path](ModelLibrary::Parsed& parsed) {
            models_->add(key,parsed);
            // The Animation stage finds its skeleton by the component's path.
            if(auto skeleton=models_->skeleton(key)) {
                std::string clips;
                for(const auto& clip:skeleton->clips) clips+=(clips.empty()?"":", ")+clip.name;
                log(path+": "+std::to_string(skeleton->skeleton->num_joints())+" joints; clips: "+(clips.empty()?"none":clips));
                animations_.put(path,std::move(skeleton));
            }
            modelsReading_.erase(key);
            --reads_;
        });
}

void Editor::readEnvironmentFile(const std::string& path, const Ogre::Vector3& scale, const std::string& key) {
    ++reads_;
    environments_[key];
    struct Read { EnvironmentImage image; std::string error; };
    const auto file=project_->resolve(path);
    tasks_.background<Read>("Reading environment "+path,[file,scale] {
        Read read;
        try { read.image=readEnvironment(file,scale); } catch(const std::exception& error) { read.error=error.what(); }
        return read;
    },[this,key](Read& read) {
        --reads_;
        if(!read.error.empty()) { log("Environment not loaded: "+read.error); return; }
        auto& slot=environments_[key];
        slot=uploadEnvironment(*root_.getRenderSystem()->getTextureGpuManager(),read.image,"ThroneEnvironment/"+key);
        prefilter(*slot->cubemap);
        // The next lighting update picks it up.
        environmentFile_.clear();
    });
}

std::filesystem::path Editor::giFolder() const {
    // scenes/exterior.scene.lton keeps its bake in scenes/exterior.gi.
    const auto path=lightingScenePath();
    const auto scene=project_ ? project_->resolve(path) : std::filesystem::path(path);
    auto name=scene.filename().u8string();
    return scene.parent_path()/std::filesystem::u8path(name.substr(0,name.find('.'))+".gi");
}

std::string Editor::lightingScenePath() const {
    if (contexts_.back().kind == Context::Kind::Scene) return contexts_.back().path;
    return doc_.world.level.scenes.empty() ? std::string() : doc_.world.level.scenes.front().scene;
}

const SceneLighting& Editor::lighting() const {
    static const SceneLighting defaults;
    const auto found = doc_.world.scenes.find(lightingScenePath());
    return found == doc_.world.scenes.end() ? defaults : found->second.lighting;
}

int Editor::giInstance() const {
    return contexts_.back().kind == Context::Kind::Prefab || view_.instances.empty() ? -1 : 0;
}

Ogre::Matrix4 Editor::instanceTransform(int instance) const {
    if (instance < 0 || std::size_t(instance) >= sceneRoots_.size()) return Ogre::Matrix4::IDENTITY;
    return sceneRoots_[std::size_t(instance)]->_getFullTransformUpdated();
}

void Editor::syncLighting(const LevelExpansion& shown) {
    const auto& level=doc_.world.level.rendering;
    if(!clustered_ || *clustered_!=level.clustered || clusterMin_!=level.minDistance || clusterMax_!=level.maxDistance) {
        if(level.clustered) scene_.setForwardClustered(true,16,8,24,96,0,16,level.minDistance,level.maxDistance);
        else scene_.setForward3D(true,4,4,5,96,level.minDistance,level.maxDistance);
        clustered_=level.clustered; clusterMin_=level.minDistance; clusterMax_=level.maxDistance;
    }
    scene_.getForwardPlus()->setDebugMode(level.debugClusters);
    // The environment and GI of the scene shown (TDDja/13).
    const auto& r=lighting();
    if(const auto path=lightingScenePath(); path!=lightingScene_) {
        lightingScene_=path;
        setSkyboxEnabled(r.sky);
    }
    sun_->setVisible(r.defaultSun);
    // An environment is read once per file, scale and temperature, which multiply its radiance.
    const auto environmentScale=toOgre(blackBody(r.environmentTemperature)*r.environmentScale);
    const auto environmentFile=r.environment.empty() || !project_ ? std::string{}
        : project_->resolve(r.environment).generic_u8string()+"#"+std::to_string(r.environmentScale)+"#"+std::to_string(r.environmentTemperature);
    if(environmentFile!=environmentFile_) {
        environmentFile_=environmentFile;
        if(!environmentFile.empty() && !environments_.count(environmentFile)) readEnvironmentFile(r.environment,environmentScale,environmentFile);
        const auto it=environments_.find(environmentFile);
        environment_=it!=environments_.end() && it->second ? &*it->second : nullptr;
        setSkyboxEnabled(skyboxEnabled_);
    }
    auto params=tonemap_->getTechnique(0)->getPass(0)->getFragmentProgramParameters();
    params->setNamedConstant("exposure",std::exp2(r.exposure));
    params->setNamedConstant("filmic",r.toneMapping?1.0f:0.0f);
    // The shown scene's bake is used while its GI is enabled, whatever settings it was made with
    // (the Inspector warns when they differ); enabling GI or showing another scene reads it again.
    const auto giScene=giInstance()>=0 ? lightingScenePath() : std::string();
    const bool giEnabled=!giScene.empty() && r.gi.enabled;
    if(!baking_ && (giScene!=giScene_ || giEnabled!=giEnabled_)) {
        giScene_=giScene;
        giEnabled_=giEnabled;
        gi_->clear();
        if(giEnabled_) readGlobalIllumination();
    }
    // The bake moves with its scene instance.
    if(gi_->active()) gi_->setTransform(instanceTransform(giInstance()));
    // Baked reflection probes (which saw the sky) take over from the environment map; a
    // material's own reflection map would otherwise be treated as one more probe.
    if(auto* reflection=environmentReflection(); reflection!=modelReflection_) {
        models_->setReflection(reflection);
        modelReflection_=reflection;
    }
    // Baked probes hold the sky and the scene's own bounce, so they replace the ambient light.
    // The environment's spherical harmonics still light what lies beyond the bake's probes.
    pbs_.setAmbientLightMode(environment_ ? Ogre::HlmsPbs::AmbientSh
        : gi_->active() ? Ogre::HlmsPbs::AmbientNone : Ogre::HlmsPbs::AmbientAutoNormal);
    // Spherical harmonics give the environment's diffuse light.
    if(environment_) scene_.setSphericalHarmonics(const_cast<Ogre::Vector3*>(environment_->harmonics.data()));
    scene_.setAmbientLight(Ogre::ColourValue(r.ambientUpper.x,r.ambientUpper.y,r.ambientUpper.z),
        Ogre::ColourValue(r.ambientLower.x,r.ambientLower.y,r.ambientLower.z),toOgre(r.ambientDirection).normalisedCopy(),
        1.0f,
        // The Adaptive Irradiance Field gives diffuse light; reflection probes only reflect.
        gi_->active() ? 0u : Ogre::SceneManager::EnvFeatures_DiffuseGiFromReflectionProbe);
    // Reserve enough Forward slots for every authored rectangular light, including hidden ones.
    unsigned approx=0,ltc=0;
    for(const auto& instance:shown.instances) for(const auto& n:instance.expansion.nodes) if(n.node.light) {
        const auto type=n.node.light->type;
        if(type==LightType::AreaApprox || type==LightType::AreaLtc) {
            approx+=areaLightModel_==0 || (areaLightModel_<0 && type==LightType::AreaApprox);
            ltc+=areaLightModel_==1 || (areaLightModel_<0 && type==LightType::AreaLtc);
        }
    }
    pbs_.setAreaLightForwardSettings(static_cast<Ogre::uint16>(approx),static_cast<Ogre::uint16>(ltc));
}

void Editor::syncTransforms(const LevelExpansion* snapshot) {
    // Before lighting, so a bake that finishes switches this frame's materials to its probes.
    tasks_.update();
    // During Play, simulation may be changing view_ on its worker thread; draw the snapshot.
    const auto& shown = snapshot ? *snapshot : view_;
    syncLighting(shown);
    // Stop or scene replacement may remove runtime nodes; discard their render resources.
    bool shrunk = sceneRoots_.size() > shown.instances.size();
    for (std::size_t i = 0; i < sceneNodes_.size() && i < shown.instances.size(); ++i)
        shrunk |= sceneNodes_[i].size() > shown.instances[i].expansion.nodes.size();
    if (shrunk) destroyRenderObjects();
    for (std::size_t i = 0; i < shown.instances.size(); ++i) {
        const auto& nodes = shown.instances[i].expansion.nodes;
        if (sceneRoots_.size() <= i) {
            sceneRoots_.push_back(scene_.getRootSceneNode()->createChildSceneNode());
            sceneNodes_.emplace_back();
        }
        // Parents come first, so each node's OGRE parent already exists.
        auto& created = sceneNodes_[i];
        for (std::size_t k = created.size(); k < nodes.size(); ++k) {
            auto* parent = nodes[k].parent == Expansion::npos ? sceneRoots_[i] : created[nodes[k].parent];
            created.push_back(parent->createChildSceneNode());
            const auto name = "ThroneEditor/Node/" + std::to_string(i) + "/" + std::to_string(k);
            auto* material = createPbsMaterial(pbs_, name);
            auto* mesh = scene_.createManualObject();
            mesh->setCastShadows(true);
            created[k]->attachObject(mesh);
            renderObjects_.push_back({i, k, created[k], mesh, material, name});
        }
    }
    for (std::size_t i = 0; i < sceneRoots_.size(); ++i) {
        const auto& transform = shown.instances[i].transform;
        sceneRoots_[i]->setPosition(toOgre(transform.position));
        sceneRoots_[i]->setOrientation(orientation(transform.rotation));
        sceneRoots_[i]->setScale(toOgre(transform.scale));
    }
    const RenderObject* outlined = nullptr;
    bool hasParticles=false;
    for (auto& render : renderObjects_) {
        const auto& expanded = shown.instances[render.instance].expansion.nodes[render.index];
        const auto& node = expanded.node;
        hasParticles|=bool(node.particles);
        const auto shape=primitiveShape(node);
        const auto surface=node.material.value_or(PbsMaterial{});
        if (shape != render.shape || surface.uvScale!=render.uvScale) {
            const auto geometry=addPrimitiveGeometry(*render.mesh,render.materialName,shape,surface.uvScale);
            render.uvScale=surface.uvScale;
            render.lower=geometry.lower; render.upper=geometry.upper; render.shape=shape;
        }
        // Nodes without a Transform inherit the nearest ancestor's placement.
        const auto transform = node.transform.value_or(Transform{});
        render.node->setPosition(toOgre(transform.position));
        render.node->setOrientation(orientation(transform.rotation));
        render.node->setScale(toOgre(transform.scale));
        render.mesh->setVisible(shape.kind != PrimitiveKind::None);
        // A Model component places its file's meshes under the node once a worker has read the
        // file; its bounds stand in for a primitive's when the node has none.
        if (node.model != render.model) {
            models_->remove(render.placedModel);
            render.model = node.model;
            render.modelKey = node.model && !node.model->path.empty() && project_
                ? ModelLibrary::key(*node.model, mediaFile(node.model->path)) : std::string{};
        }
        if (!render.modelKey.empty() && !render.placedModel.root) {
            if (models_->has(render.modelKey)) {
                render.placedModel = models_->place(render.modelKey, *render.node);
                revealModels();
                // A file without meshes is not tried again.
                if (!render.placedModel.root) render.modelKey.clear();
            } else if (modelsReading_.insert(render.modelKey).second) readModel(*node.model, render.modelKey);
        }
        if (render.placedModel.root && shape.kind == PrimitiveKind::None)
            render.lower = render.placedModel.lower, render.upper = render.placedModel.upper;
        // An Animator poses a skinned model's bones as the simulation last published; OGRE's
        // workers then derive their matrices, and the vertex shader skins the mesh.
        // Each skinned item (a file's skinned submeshes share one skeleton definition) has its
        // own instance of it.
        const auto* pose = node.animator ? animation_.published(render.instance, render.index) : nullptr;
        for (auto* item : pose ? render.placedModel.items : std::vector<Ogre::Item*>{}) {
            auto* skeleton = item->getSkeletonInstance();
            if (!skeleton) continue;
            if (render.bonesOf != skeleton->getDefinition() || render.bonesFor != pose->asset) {
                // ozz orders the joints itself: match them to the mesh's bones by name.
                std::map<std::string, std::size_t> byName;
                for (std::size_t b = 0; b < skeleton->getNumBones(); ++b) byName[skeleton->getBone(b)->getName()] = b;
                const auto names = pose->asset->skeleton->joint_names();
                render.bones.assign(names.size(), -1);
                for (std::size_t j = 0; j < names.size(); ++j)
                    if (const auto found = byName.find(names[j]); found != byName.end()) render.bones[j] = int(found->second);
                render.bonesOf = skeleton->getDefinition(), render.bonesFor = pose->asset;
            }
            for (std::size_t j = 0; j < render.bones.size() && j < pose->joints.size(); ++j) {
                if (render.bones[j] < 0) continue;
                auto* bone = skeleton->getBone(std::size_t(render.bones[j]));
                const auto& joint = pose->joints[j];
                bone->setPosition(Ogre::Vector3(joint.position[0], joint.position[1], joint.position[2]));
                bone->setOrientation(Ogre::Quaternion(joint.rotation[3], joint.rotation[0], joint.rotation[1], joint.rotation[2]));
                bone->setScale(Ogre::Vector3(joint.scale[0], joint.scale[1], joint.scale[2]));
            }
        }
        const auto colour=primitiveColour(node);
        render.material->setDiffuse(Ogre::Vector3(colour[0],colour[1],colour[2]));
        render.material->setRoughness(surface.roughness);
        render.material->setWorkflow(surface.metalness<0?Ogre::HlmsPbsDatablock::SpecularWorkflow:Ogre::HlmsPbsDatablock::MetallicWorkflow);
        if(surface.metalness<0) render.material->setFresnel(Ogre::Vector3(surface.fresnel.data()),false);
        else render.material->setMetalness(surface.metalness);
        render.material->setEmissive(Ogre::Vector3(surface.emissive.data()));
        const auto previous=render.surface.value_or(PbsMaterial{});
        const auto texture=[&](Ogre::PbsTextureTypes slot,const std::string& path,const std::string& old,int kind) {
            if(path!=old) render.material->setTexture(slot,projectTexture(path,kind));
        };
        texture(Ogre::PBSM_DIFFUSE,surface.diffuseMap,previous.diffuseMap,Ogre::CommonTextureTypes::Diffuse);
        texture(Ogre::PBSM_NORMAL,surface.normalMap,previous.normalMap,Ogre::CommonTextureTypes::NormalMap);
        texture(Ogre::PBSM_ROUGHNESS,surface.roughnessMap,previous.roughnessMap,Ogre::CommonTextureTypes::Monochrome);
        texture(Ogre::PBSM_SPECULAR,surface.specularMap,previous.specularMap,Ogre::CommonTextureTypes::Diffuse);
        // Without a reflection map of its own, a surface reflects the level's environment.
        auto* reflection=surface.reflectionMap.empty() ? environmentReflection()
            : projectTexture(surface.reflectionMap,Ogre::CommonTextureTypes::EnvMap);
        if(render.material->getTexture(Ogre::PBSM_REFLECTION)!=reflection) render.material->setTexture(Ogre::PBSM_REFLECTION,reflection);
        // Changing these rebuilds the shaders, so only when they change. Hash's opacity is the
        // material's transparency without blending (HlmsPbs multiplies the diffuse alpha by it).
        if(surface.alpha!=previous.alpha || surface.alphaThreshold!=previous.alphaThreshold || surface.opacity!=previous.opacity) {
            render.material->setAlphaTest(surface.alpha==AlphaMode::Test ? Ogre::CMPF_GREATER_EQUAL : Ogre::CMPF_ALWAYS_PASS);
            render.material->setAlphaTestThreshold(surface.alphaThreshold);
            render.material->setAlphaHashing(surface.alpha==AlphaMode::Hash);
            render.material->setTransparency(surface.alpha==AlphaMode::Hash ? surface.opacity : 1.0f,
                                             Ogre::HlmsPbsDatablock::None, true, false);
        }
        // Translucency and subsurface scattering are a custom piece named after its content, so
        // equal ones share shaders.
        if(surface.translucency!=previous.translucency || surface.subsurfaceWidth!=previous.subsurfaceWidth) {
            const auto piece=surfacePiece(surface.translucency,surface.subsurfaceWidth);
            render.material->setCustomPieceCodeFromMemory(piece.empty() ? std::string() : "Throne/"+std::to_string(std::hash<std::string>{}(piece)),
                                                          piece,Ogre::CustomPieceStage::PixelShader);
        }
        render.surface=surface;
        if(node.light && !render.light) {
            if(!render.lightNode) {
                render.lightNode=render.node->createChildSceneNode();
                render.lightNode->setFixedYawAxis(false);
            }
            render.light=scene_.createLight(); render.lightNode->attachObject(render.light);
        }
        if(!node.light && render.light) { scene_.destroyLight(render.light); render.light=nullptr; }
        if(node.light) {
            auto c=*node.light; auto* light=render.light;
            if(areaLightModel_>=0 && (c.type==LightType::AreaApprox || c.type==LightType::AreaLtc))
                c.type=areaLightModel_==0?LightType::AreaApprox:LightType::AreaLtc;
            const Ogre::Light::LightTypes types[]={Ogre::Light::LT_DIRECTIONAL,Ogre::Light::LT_POINT,Ogre::Light::LT_SPOTLIGHT,Ogre::Light::LT_AREA_APPROX,Ogre::Light::LT_AREA_LTC};
            light->setType(types[static_cast<int>(c.type)]);
            light->setVisible(c.enabled); light->setPowerScale(c.power);
            const auto tint=blackBody(c.temperature);
            light->setDiffuseColour(c.colour[0]*tint.x,c.colour[1]*tint.y,c.colour[2]*tint.z);
            light->setSpecularColour(c.colour[0]*tint.x,c.colour[1]*tint.y,c.colour[2]*tint.z);
            // Light::setDirection rotates its parent; reset the dedicated child to avoid roll drift.
            render.lightNode->setOrientation(Ogre::Quaternion::IDENTITY);
            light->setDirection(toOgre(c.direction).normalisedCopy());
            light->setCastShadows(shadowsEnabled_ && (c.type==LightType::Directional ||
                (c.castShadows && (c.type==LightType::Point || c.type==LightType::Spot))));
            light->setAttenuationBasedOnRadius(c.radius,c.threshold);
            light->setSpotlightRange(Ogre::Degree(c.innerAngle),Ogre::Degree(c.outerAngle));
            light->setRectSize(Ogre::Vector2(c.width,c.height)); light->setDoubleSided(c.twoSided);
            const bool area=c.type==LightType::AreaApprox || c.type==LightType::AreaLtc;
            auto* mask=c.type==LightType::AreaApprox?projectTexture(c.mask,Ogre::CommonTextureTypes::Monochrome,true):nullptr;
            if(mask) light->setTexture(mask); else light->mTextureLightMaskIdx=std::numeric_limits<Ogre::uint16>::max();
            if(area && !render.emitter) {
                const auto name=render.materialName+"/Emitter";
                render.emitterMaterial=createMaterial(unlit_,name);
                // The R8 area mask stores intensity, so replicate red into every channel.
                render.emitterMaterial->setTextureSwizzle(0,Ogre::HlmsUnlitDatablock::R_MASK,
                    Ogre::HlmsUnlitDatablock::R_MASK,Ogre::HlmsUnlitDatablock::R_MASK,Ogre::HlmsUnlitDatablock::R_MASK);
                render.emitter=scene_.createManualObject(); render.emitter->setCastShadows(false);
                render.emitter->setRenderQueueGroup(unlitRenderQueue);
                render.emitterNode=render.lightNode->createChildSceneNode(); render.emitterNode->attachObject(render.emitter);
            }
            if(render.emitter) {
                render.emitter->setVisible(area && c.showEmitter && c.enabled);
                PrimitiveShape rect; rect.kind=PrimitiveKind::Plane; rect.width=c.width; rect.depth=c.height;
                if(rect!=render.emitterShape) {
                    addPrimitiveGeometry(*render.emitter,render.materialName+"/Emitter",rect);
                    render.emitterShape=rect;
                }
                render.emitterNode->setOrientation(Ogre::Quaternion(Ogre::Degree(-90),Ogre::Vector3::UNIT_X));
                render.emitterMaterial->setColour(Ogre::ColourValue(c.colour[0],c.colour[1],c.colour[2]));
                render.emitterMaterial->setTexture(0,mask);
            }
        } else if(render.emitter) render.emitter->setVisible(false);
        if (selected_.kind == Selection::Kind::Node && selected_.instance == static_cast<int>(render.instance) &&
            selected_.path == expanded.path)
            outlined = &render;
    }
    if(hasParticles || particleSystemCount_) syncParticles(shown);
    // The sky's sun follows the default sun, or else the first directional Light component.
    Ogre::Light* skySun=sun_->getVisible()?sun_:nullptr;
    for(const auto& render:renderObjects_)
        if(!skySun && render.light && render.light->getVisible() && render.light->getType()==Ogre::Light::LT_DIRECTIONAL) skySun=render.light;
    if(skySun) {
        const auto direction=skySun->getDerivedDirectionUpdated();
        sky_->setSunDir(direction,std::asin(std::clamp(-direction.y,-1.0f,1.0f))/Ogre::Math::PI);
    }
    gridNode_->setVisible(showGrid_);
    if (gi_->debugVisible()) updateProbeLayout();
    // Drawn in the scene's coordinates, under a node placed as its instance.
    if (const auto instance = giInstance(); instance >= 0) {
        Ogre::Vector3 position, scale;
        Ogre::Quaternion rotation;
        instanceTransform(instance).decomposition(position, scale, rotation);
        giBoxesNode_->setPosition(position);
        giBoxesNode_->setOrientation(rotation);
        giBoxesNode_->setScale(scale);
    }
    if (gi_->debugVisible() && (gi_->version() != giBoxesVersion_ || layoutVersion_ != giBoxesLayout_)) {
        giBoxesVersion_ = gi_->version();
        giBoxesLayout_ = layoutVersion_;
        giBoxes_->clear();
        const auto& gi = lighting().gi;
        const auto box = [](const Vec3& centre, const Vec3& half, std::vector<Ogre::Vector3>& into) {
            for (const auto& corner : cubeEdges()) into.push_back(toOgre(centre) + corner * toOgre(half) * 2.0f);
        };
        // The probes' volume (the box, or the scene's bounds) in white, the bake's reflection
        // probe areas in blue, then the Adaptive Irradiance Field's probes as crosses coloured by
        // the finest level of the cells around them: yellow (finest), green, cyan, blue, violet,
        // red; probes interpolated from a coarser cell (not rendered) in grey.
        std::vector<Ogre::Vector3> lines;
        box((layoutLower_ + layoutUpper_) * .5f, (layoutUpper_ - layoutLower_) * .5f, lines);
        addLines(*giBoxes_, lines, Ogre::ColourValue::White);
        lines.clear();
        for (const auto& [centre, half] : gi_->reflectionAreas()) box(centre, half, lines);
        if (!lines.empty()) addLines(*giBoxes_, lines, Ogre::ColourValue(0.3f, 0.6f, 1.0f));
        const Ogre::ColourValue colours[ProbeOctree::levels + 1] = {
            Ogre::ColourValue(1, .85f, .2f), Ogre::ColourValue(.4f, 1, .4f), Ogre::ColourValue(.3f, .9f, 1),
            Ogre::ColourValue(.3f, .5f, 1),  Ogre::ColourValue(.8f, .4f, 1), Ogre::ColourValue(1, .35f, .35f),
            Ogre::ColourValue(.5f, .5f, .5f)};
        for (unsigned colour = 0; colour <= ProbeOctree::levels; ++colour) {
            lines.clear();
            for (const auto& probe : octree_.probes)
                if ((probe.from >= 0 ? ProbeOctree::levels : probe.level) == colour)
                    for (int axis = 0; axis < 3; ++axis) {
                        Ogre::Vector3 arm(0.0f);
                        arm[axis] = 0.15f;
                        const auto at = toOgre(octree_.position(probe));
                        lines.insert(lines.end(), {at - arm, at + arm});
                    }
            if (!lines.empty()) addLines(*giBoxes_, lines, colours[colour]);
        }
        if (showSurface_) {
            // Each surface point as a small cross: red when it places probes, grey when its object
            // is below Min Object Volume.
            std::vector<Ogre::Vector3> left;
            lines.clear();
            for (const auto& point : sceneSurface(giInstance())) {
                auto& into = point.volume < gi.minObjectVolume ? left : lines;
                for (int axis = 0; axis < 3; ++axis) {
                    Ogre::Vector3 arm(0.0f);
                    arm[axis] = 0.1f;
                    into.insert(into.end(), {toOgre(point.position) - arm, toOgre(point.position) + arm});
                }
            }
            if (!lines.empty()) addLines(*giBoxes_, lines, Ogre::ColourValue(1, .2f, .2f));
            if (!left.empty()) addLines(*giBoxes_, left, Ogre::ColourValue(.5f, .5f, .5f));
        }
    }
    giBoxesNode_->setVisible(gi_->debugVisible() && giInstance() >= 0);
    outlineNode_->setVisible(outlined != nullptr);
    if (outlined) {
        const auto centre=(outlined->lower+outlined->upper)*.5f;
        const auto size=outlined->upper-outlined->lower;
        outlineNode_->setPosition(outlined->node->_getFullTransformUpdated().transformAffine(toOgre(centre)));
        outlineNode_->setOrientation(outlined->node->_getDerivedOrientationUpdated());
        outlineNode_->setScale(outlined->node->_getDerivedScaleUpdated() * toOgre(Vec3{(std::max)(size.x,.02f),(std::max)(size.y,.02f),(std::max)(size.z,.02f)}) * 1.02f);
    }
}

void Editor::checkpoint() { commit(doc_); }

void Editor::commit(Document before) {
    undo_.push_back(std::move(before));
    if (undo_.size() > historyLimit) undo_.erase(undo_.begin());
    redo_.clear();
    dirty_ = true;
}

void Editor::undo() {
    if (undo_.empty()) return;
    redo_.push_back(std::move(doc_));
    doc_ = std::move(undo_.back());
    undo_.pop_back();
    dirty_ = true;
}

void Editor::redo() {
    if (redo_.empty()) return;
    undo_.push_back(std::move(doc_));
    doc_ = std::move(redo_.back());
    redo_.pop_back();
    dirty_ = true;
}

void Editor::addNode(PrimitiveKind kind) {
    // Use the smallest available suffix for this primitive type.
    std::string name;
    for (int number = 1;; ++number) {
        const auto base=kind==PrimitiveKind::Sphere?"Sphere":kind==PrimitiveKind::Capsule?"Capsule":kind==PrimitiveKind::Plane?"Plane":"Cube";
        name = std::string(base) + " " + std::to_string(number);
        const auto taken = [&](const LevelExpansion::Instance& instance) {
            return std::any_of(instance.expansion.nodes.begin(), instance.expansion.nodes.end(),
                               [&](const ExpandedNode& node) { return node.node.name == name; });
        };
        if (std::none_of(view_.instances.begin(), view_.instances.end(), taken)) break;
    }
    SceneNode node;
    node.name = name;
    node.transform = Transform{{0, 0.5f, 0}};
    setPrimitive(node,kind);
    if(kind==PrimitiveKind::Box) node.boxCollider.emplace();
    if(kind==PrimitiveKind::Sphere) node.sphereCollider.emplace();
    if(kind==PrimitiveKind::Capsule) node.capsuleCollider.emplace();
    if(kind==PrimitiveKind::Plane) node.planeCollider.emplace();
    auto before = doc_;
    if (insert(std::move(node))) commit(std::move(before));
}

void Editor::addInstance() {
    const std::string prefab = newPrefabPath_;
    if (prefab.empty()) return;
    auto before = doc_;
    // A prefab file that does not exist yet starts with a root node and is written by save().
    if (!doc_.prefabs.count(prefab) && !std::filesystem::exists(project_->resolve(prefab))) {
        Prefab made;
        made.addNode(std::filesystem::u8path(prefab).stem().stem().u8string());
        doc_.prefabs[prefab] = made;
        library_.put(prefab, made);
    }
    // Expanding a probe in the edited file's place reports a broken prefab or a cycle.
    Prefab probe;
    probe.addNode("probe").prefab = prefab;
    const auto check = expand(probe, library_, contexts_.back().kind == Context::Kind::Prefab ? contexts_.back().path : "");
    SceneNode node;
    node.name = std::filesystem::u8path(prefab).stem().stem().u8string();
    node.prefab = prefab;
    node.transform.emplace();
    if (!check.errors.empty() || !insert(std::move(node))) {
        if (!check.errors.empty()) log("Cannot place " + prefab + ": " + check.errors.front());
        if (!before.prefabs.count(prefab)) library_.forget(prefab);
        doc_ = std::move(before);
        return;
    }
    commit(std::move(before));
}

bool Editor::insert(SceneNode node) {
    node.id = newNodeId();
    node.parent = 0;
    const int instance = std::max(selected_.instance, 0);
    // Into an instance, the new node is kept in the overrides of the instance that
    // holds the selected node: under it, or under the instance itself.
    Overrides* into = nullptr;
    NodePath parent, prefix;
    NodeTarget target;
    if (selectedNode()) {
        target = targetOf(selected_.instance, selected_.path);
        if (target.node && !target.node->prefab.empty()) {
            into = &target.node->overrides;
            prefix = selected_.path;
        } else if (target.addedTo) {
            into = target.addedTo;
            parent = {target.node->id};
            prefix.assign(selected_.path.begin(), selected_.path.end() - 1);
        } else if (!target.node && target.overrides) {
            into = target.overrides;
            parent = target.relative;
            prefix.assign(selected_.path.begin(), selected_.path.end() - static_cast<std::ptrdiff_t>(parent.size()));
        }
    } else if (content() && content()->variant()) {
        into = &content()->overrides;  // A variant adds under its base's root.
    }
    if (into) {
        into->added.push_back({parent, node});
        selected_ = {Selection::Kind::Node, instance, prefix};
        selected_.path.push_back(node.id);
        return true;
    }
    // At the level, into the scene of the selected scene instance or node.
    auto* edited = selectedNode() || selectedScene() ? fileOf(selected_.instance) : content();
    if (!edited) {
        log("Select a scene instance, or a node in one, to add to it");
        return false;
    }
    // Into the edited file: under the selected plain node, else under a prefab's single root.
    NodeId under = 0;
    if (target.node && target.node->prefab.empty()) under = target.node->id;
    else if (contexts_.back().kind == Context::Kind::Prefab && !edited->nodes.empty()) under = edited->nodes.front().id;
    auto& added = edited->addNode(node.name, under);
    node.id = added.id;
    node.parent = added.parent;
    added = std::move(node);
    selected_ = {Selection::Kind::Node, instance, {added.id}};
    return true;
}

bool Editor::isPrefabRoot(NodeId id) {
    const auto* node = content() ? content()->findNode(id) : nullptr;
    return node && node->parent == 0 && contexts_.back().kind == Context::Kind::Prefab;
}

bool Editor::isVariant(const std::string& prefab) {
    try {
        return library_.prefab(prefab).variant();
    } catch (const std::exception&) {
        return false;
    }
}

void Editor::prefabify(int instance, NodeId id, std::string file) {
    auto* edited = fileOf(instance);
    auto* node = edited ? edited->findNode(id) : nullptr;
    if (!node || edited->variant()) return;
    if (isPrefabRoot(id)) {
        log("The root of a prefab cannot become a prefab; make a variant of the prefab instead");
        return;
    }
    if (file == contexts_.back().path) {
        log("A prefab cannot be replaced by one of its own nodes");
        return;
    }
    // prefabs/<name>.prefab.lton, numbered when taken; characters a path cannot hold become '_'.
    if (file.empty()) {
        std::string stem = node->name;
        for (auto& character : stem)
            if (std::string_view("<>:\"/\\|?*").find(character) != std::string_view::npos || character < 32)
                character = '_';
        for (int number = 1;; ++number) {
            file = "prefabs/" + stem + (number > 1 ? " " + std::to_string(number) : "") + ".prefab.lton";
            if (!doc_.prefabs.count(file) && !std::filesystem::exists(project_->resolve(file))) break;
        }
    }
    // An instance becomes a variant of its prefab with its overrides; a plain node takes its subtree along.
    Prefab made;
    std::set<NodeId> moved;
    if (!node->prefab.empty()) {
        made.base = node->prefab;
        made.overrides = node->overrides;
    } else {
        for (const auto& each : edited->nodes)
            if (edited->isWithin(each.id, id)) made.nodes.push_back(each);
        made.nodes.front().parent = 0;
        for (const auto& each : made.nodes)
            if (each.id != id) moved.insert(each.id);
    }
    // Replacing a prefab with a node that contains an instance of it would make a cycle.
    if (const auto check = expand(made, library_, file); !check.errors.empty()) {
        log("Cannot save " + file + ": " + check.errors.front());
        return;
    }
    try {
        made.save(project_->resolve(file));
    } catch (const std::exception& error) {
        log("Could not write " + file + ": " + error.what());
        return;
    }
    auto before = doc_;
    // A copy opened for editing would write the old content back on save.
    doc_.prefabs.erase(file);
    library_.forget(file);
    // Paths and references into the moved nodes still find them inside the new instance (see NodePath).
    for (const auto child : moved)
        if (auto* each = edited->findNode(child); each && each->parent == id) edited->removeNode(child);
    node = edited->findNode(id);
    node->prefab = file;
    node->overrides = {};
    selected_ = {Selection::Kind::Node, instance, {id}};
    commit(std::move(before));
    log("Saved " + file);
}

void Editor::dropOnPrefab(int instance, NodeId id, const std::string& file) {
    auto* edited = fileOf(instance);
    auto* node = edited ? edited->findNode(id) : nullptr;
    if (!node) return;
    if (node->prefab == file) applyOverrides(*node, node->overrides);
    else replacing_ = Replace{instance, id, file};
}

namespace {
/// Copies the properties @p from sets into @p into.
void assign(NodeOverride& into, const NodeOverride& from) {
    forEachProperty([](const char*, auto& kept, const auto& set) { overlay(kept, set); }, into, from);
}

/// Adds @p from on top of the overrides @p into already has.
void merge(Overrides& into, const Overrides& from) {
    for (const auto& entry : from.nodes) assign(into.node(entry.path), entry);
    for (const auto& entry : from.systems)
        for (const auto& [name, value] : entry.fields) into.fieldsOf(entry.instance, entry.system, entry.behaviour).fields[name] = value;
    into.added.insert(into.added.end(), from.added.begin(), from.added.end());
    into.removed.insert(into.removed.end(), from.removed.begin(), from.removed.end());
    into.addedSystems.insert(into.addedSystems.end(), from.addedSystems.begin(), from.addedSystems.end());
    into.removedSystems.insert(into.removedSystems.end(), from.removedSystems.begin(), from.removedSystems.end());
}
}

void Editor::applyOverrides(SceneNode& holder, Overrides piece) {
    if (holder.prefab.empty() || piece.empty()) return;
    Prefab prefab;
    try {
        prefab = doc_.prefabs.count(holder.prefab) ? doc_.prefabs[holder.prefab] : library_.prefab(holder.prefab);
    } catch (const std::exception& error) {
        log(error.what());
        return;
    }
    // A variant keeps them as its own overrides; a prefab writes them into its nodes,
    // or into the overrides of its own instances for what lies deeper.
    if (prefab.variant()) {
        merge(prefab.overrides, piece);
    } else {
        for (const auto& entry : piece.nodes) {
            auto* target = entry.path.empty() ? &prefab.nodes.front() : prefab.findNode(entry.path[0]);
            if (!target) continue;
            if (entry.path.size() <= 1) {
                applyOverride(*target, entry);
            } else {
                auto inner = entry;
                inner.path.erase(inner.path.begin());
                assign(target->overrides.node(inner.path), inner);
            }
        }
        // Added nodes join the prefab, or the overrides of its instance they were added inside.
        for (auto entry : piece.added) {
            if (entry.parent.size() > 1) {
                if (auto* target = prefab.findNode(entry.parent[0])) {
                    entry.parent.erase(entry.parent.begin());
                    target->overrides.added.push_back(std::move(entry));
                }
                continue;
            }
            entry.node.parent = entry.parent.empty() ? prefab.nodes.front().id : entry.parent[0];
            if (prefab.findNode(entry.node.parent)) prefab.nodes.push_back(std::move(entry.node));
        }
        // Added systems join the prefab's; removed ones leave it, or the instance they come from.
        for (const auto& system : piece.addedSystems) {
            auto& made = prefab.addSystem(system.name);
            const auto madeId = made.id;
            made = system;
            made.id = madeId;
        }
        for (const auto& entry : piece.removedSystems) {
            if (entry.instance.empty()) {
                prefab.removeSystem(entry.system);
            } else if (auto* target = prefab.findNode(entry.instance[0])) {
                target->overrides.removedSystems.push_back({NodePath(entry.instance.begin() + 1, entry.instance.end()), entry.system});
            }
        }
        // Removed nodes leave the prefab, or join the removals of its instance they lie in.
        for (const auto& path : piece.removed) {
            if (path.size() == 1) {
                prefab.removeNode(path[0]);
            } else if (auto* target = prefab.findNode(path[0])) {
                target->overrides.removed.emplace_back(path.begin() + 1, path.end());
            }
        }
        for (const auto& entry : piece.systems) {
            if (entry.instance.empty()) {
                if (auto* system = prefab.findSystem(entry.system); system && entry.behaviour < system->behaviour.size())
                    for (const auto& [name, value] : entry.fields) system->behaviour[entry.behaviour].data[name] = value;
            } else if (auto* target = prefab.findNode(entry.instance[0])) {
                const NodePath rest(entry.instance.begin() + 1, entry.instance.end());
                for (const auto& [name, value] : entry.fields)
                    target->overrides.fieldsOf(rest, entry.system, entry.behaviour).fields[name] = value;
            }
        }
    }
    try {
        prefab.save(project_->resolve(holder.prefab));
    } catch (const std::exception& error) {
        log("Could not write " + holder.prefab + ": " + error.what());
        return;
    }
    auto before = doc_;
    if (doc_.prefabs.count(holder.prefab)) doc_.prefabs[holder.prefab] = prefab;
    library_.forget(holder.prefab);
    holder.overrides.subtract(piece);
    commit(std::move(before));
    log("Applied the overrides to " + holder.prefab);
}

void Editor::unpack(int instance, NodeId id) {
    auto* edited = fileOf(instance);
    auto* node = edited ? edited->findNode(id) : nullptr;
    if (!node || node->prefab.empty()) return;
    const auto expansion = view_.instances[instance].expansion;
    const auto top = expansion.find({id});
    if (top == Expansion::npos) return;
    // The instance's nodes, parents first: the instance itself, then everything below it.
    std::vector<std::size_t> inside{top};
    for (auto i = top + 1; i < expansion.nodes.size(); ++i)
        for (auto up = expansion.nodes[i].parent; up != Expansion::npos; up = expansion.nodes[up].parent)
            if (up == top) {
                inside.push_back(i);
                break;
            }
    // Each keeps its ID unless the file, or an earlier copy of the same prefab, already uses it.
    std::set<NodeId> used;
    for (const auto& each : edited->nodes) used.insert(each.id);
    std::map<std::size_t, NodeId> ids{{top, id}};
    bool renamed = false;
    for (const auto i : inside) {
        if (i == top) continue;
        auto wanted = expansion.nodes[i].node.id;
        for (; !used.insert(wanted).second; renamed = true) wanted = newNodeId();
        ids[i] = wanted;
    }
    std::vector<SceneNode> plain;
    for (const auto i : inside) {
        auto each = expansion.nodes[i].node;
        each.id = ids[i];
        each.parent = i == top ? node->parent : ids.at(expansion.nodes[i].parent);
        each.prefab.clear();
        each.overrides = {};
        plain.push_back(std::move(each));
    }
    auto before = doc_;
    // A reference into the instance (relative to @p base) now names its node directly.
    const auto direct = [&](NodeRef& ref, const NodePath& base) {
        if (!ref.node) return;
        auto path = base;
        path.push_back(ref.node);
        path.insert(path.end(), ref.inner.begin(), ref.inner.end());
        if (const auto it = ids.find(expansion.find(path)); it != ids.end()) ref = {ref.scene, it->second, {}};
    };
    const auto refs = [&](std::map<std::string, FieldValue>& fields, std::uint32_t scene, const NodePath& base) {
        for (auto& [name, value] : fields)
            if (auto* ref = std::get_if<NodeRef>(&value); ref && ref->scene == scene) direct(*ref, base);
    };
    for (std::size_t k = 0; k < inside.size(); ++k) {
        auto base = expansion.nodes[inside[k]].path;
        if (!base.empty() && !expansion.nodes[inside[k]].root) base.pop_back();
        for (auto* list : {&plain[k].behaviour, &plain[k].renderEvent})
            for (auto& attachment : *list) refs(attachment.data, 0, base);
    }
    for (auto& each : edited->nodes) {
        for (auto* list : {&each.behaviour, &each.renderEvent})
            for (auto& attachment : *list) refs(attachment.data, 0, {});
    }
    // The instance's systems become the file's, by the instance they came from and their ID.
    std::map<std::pair<std::size_t, std::uint32_t>, std::uint32_t> systemIds;
    for (const auto& expanded : expansion.systems) {
        const auto from = expansion.find(expanded.instance);
        if (expanded.instance.empty() || !ids.count(from)) continue;
        auto& made = edited->addSystem(expanded.system.name);
        const auto madeId = made.id;
        made = expanded.system;
        made.id = madeId;
        for (auto& behaviour : made.behaviour) refs(behaviour.data, 0, expanded.instance);
        systemIds[{from, expanded.system.id}] = madeId;
    }
    for (auto& system : edited->systems)
        for (auto& behaviour : system.behaviour) refs(behaviour.data, 0, {});
    // Level systems refer to nodes of the level's instances of this scene.
    for (const auto& placed : doc_.world.level.scenes) {
        const auto scene = doc_.world.scenes.find(placed.scene);
        if (scene == doc_.world.scenes.end() || &scene->second != edited) continue;
        for (auto& system : doc_.world.level.systems)
            for (auto& behaviour : system.behaviour) refs(behaviour.data, placed.id, {});
    }
    // The unpacked nodes take the instance node's place among its siblings.
    const auto at = std::find_if(edited->nodes.begin(), edited->nodes.end(), [id](const SceneNode& each) { return each.id == id; });
    const auto position = edited->nodes.erase(at);
    edited->nodes.insert(position, plain.begin(), plain.end());
    selected_ = {Selection::Kind::Node, instance, {id}};
    commit(std::move(before));
    if (renamed && contexts_.back().kind == Context::Kind::Prefab)
        log("Some unpacked nodes got new IDs; overrides elsewhere that pointed at them no longer apply");
}

void Editor::replaceDialog() {
    if (replacing_) {
        ImGui::OpenPopup("Replace Prefab");
        pendingReplace_ = std::move(replacing_);
        replacing_.reset();
    }
    if (!ImGui::BeginPopupModal("Replace Prefab", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("Replace %s with this node?", pendingReplace_ ? pendingReplace_->file.c_str() : "");
    ImGui::TextDisabled("Every instance of it will show the new content.");
    if (ImGui::Button("Replace")) {
        ImGui::CloseCurrentPopup();
        if (pendingReplace_) prefabify(pendingReplace_->instance, pendingReplace_->id, pendingReplace_->file);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Editor::addSystem() {
    // Into an instance (the selected system's, or the one holding the selected
    // node), the system is kept in that instance's overrides.
    Overrides* into = nullptr;
    NodePath prefix;
    if (selected_.kind == Selection::Kind::System && selected_.instance >= 0) {
        const auto target = systemTarget();
        if (target.addedTo) {
            into = target.addedTo;
            prefix = selected_.path;
        } else if (target.overrides) {
            into = target.overrides;
            prefix.assign(selected_.path.begin(), selected_.path.end() - static_cast<std::ptrdiff_t>(target.instance.size()));
        }
    } else if (selectedNode()) {
        const auto target = targetOf(selected_.instance, selected_.path);
        if (target.node && !target.node->prefab.empty()) {
            into = &target.node->overrides;
            prefix = selected_.path;
        } else if (target.addedTo) {
            into = target.addedTo;
            prefix.assign(selected_.path.begin(), selected_.path.end() - 1);
        } else if (!target.node && target.overrides) {
            into = target.overrides;
            prefix.assign(selected_.path.begin(), selected_.path.end() - static_cast<std::ptrdiff_t>(target.relative.size()));
        }
    } else if (content() && content()->variant()) {
        into = &content()->overrides;
    }
    if (into) {
        auto before = doc_;
        System system{newSystemId(), "System", {}};
        into->addedSystems.push_back(system);
        selected_ = {Selection::Kind::System, std::max(selected_.instance, 0), prefix, system.id};
        commit(std::move(before));
        return;
    }
    try {
        auto before = doc_;
        // At the level, to the scene of the selection when it lies in one; else to the level.
        auto* file = selected_.kind != Selection::Kind::None ? fileOf(selected_.instance) : content();
        auto& owner = file ? static_cast<SystemSet&>(*file) : doc_.world.level;
        auto& added = owner.addSystem("System");
        added.name = "System " + std::to_string(added.id);
        selected_ = {Selection::Kind::System, file ? std::max(selected_.instance, 0) : -1, {}, added.id};
        commit(std::move(before));
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Editor::addScene() {
    const std::string scene = newScenePath_;
    if (scene.empty() || content()) return;
    try {
        auto before = doc_;
        if (!doc_.world.scenes.count(scene)) {
            const auto file = project_->resolve(scene);
            doc_.world.scenes[scene] = std::filesystem::exists(file) ? Scene::load(file) : Scene();
        }
        doc_.world.level.addScene(scene);
        commit(std::move(before));
    } catch (const std::exception& error) {
        log("Could not add " + scene + ": " + error.what());
    }
}

void Editor::removeScene(std::uint32_t id) {
    auto& scenes = doc_.world.level.scenes;
    checkpoint();
    scenes.erase(std::remove_if(scenes.begin(), scenes.end(), [id](const SceneInstance& each) { return each.id == id; }),
                 scenes.end());
    selected_ = {};
}

void Editor::duplicate() {
    const auto target = targetOf(selected_.instance, selected_.path);
    if (!target.node || target.addedTo || isPrefabRoot(target.node->id)) return;
    try {
        auto before = doc_;
        auto* file = fileOf(selected_.instance);
        auto copy = *target.node;
        const auto original = copy.id;
        auto& added = file->addNode(copy.name, copy.parent);
        copy.id = added.id;
        if (copy.name.size() <= 250) copy.name += " Copy";
        if (copy.transform) copy.transform->position.x += 1;
        added = std::move(copy);
        const auto remap = [&](Behaviour& attachment) {
            for (auto& [name, value] : attachment.data)
                if (auto* ref = std::get_if<NodeRef>(&value); ref && !ref->scene && ref->node == original)
                    ref->node = added.id;
        };
        for (auto* list : {&added.behaviour, &added.renderEvent})
            for (auto& attachment : *list) remap(attachment);
        // Systems that refer to the node come along, referring to the copy instead
        // (the file's own, and at the level the level's naming this scene instance).
        const auto copySystems = [&](SystemSet& owner, std::uint32_t scene) {
            for (std::size_t i = 0, count = owner.systems.size(); i < count; ++i) {
                auto system = owner.systems[i];
                bool refers = false;
                for (auto& behaviour : system.behaviour)
                    for (auto& [name, value] : behaviour.data)
                        if (auto* ref = std::get_if<NodeRef>(&value); ref && ref->scene == scene && ref->node == original) {
                            ref->node = added.id;
                            refers = true;
                        }
                if (!refers) continue;
                auto& made = owner.addSystem(system.name);
                system.id = made.id;
                if (system.name.size() <= 250) system.name += " Copy";
                made = std::move(system);
            }
        };
        const auto copied = added.id;
        copySystems(*file, 0);
        if (!content())
            if (const auto* placed = doc_.world.level.findScene(view_.instances[selected_.instance].id))
                copySystems(doc_.world.level, placed->id);
        selected_.path = {copied};
        commit(std::move(before));
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Editor::erase() {
    if (const auto* placed = selectedScene()) {
        removeScene(placed->id);
        return;
    }
    if (selected_.kind == Selection::Kind::Node) {
        const auto target = targetOf(selected_.instance, selected_.path);
        // An added node leaves its instance's overrides, with the nodes added under it.
        if (target.addedTo) {
            checkpoint();
            std::set<NodeId> gone{target.node->id};
            auto& added = target.addedTo->added;
            for (std::size_t before = 0; before != gone.size();) {
                before = gone.size();
                for (const auto& entry : added)
                    if (!entry.parent.empty() && gone.count(entry.parent[0])) gone.insert(entry.node.id);
            }
            added.erase(std::remove_if(added.begin(), added.end(),
                                       [&](const AddedNode& entry) { return gone.count(entry.node.id) > 0; }),
                        added.end());
            selected_ = {};
            return;
        }
        // A node that comes from an instance is removed by that instance's overrides.
        if (!target.node) {
            if (!target.overrides || target.relative.empty()) return;
            checkpoint();
            target.overrides->removed.push_back(target.relative);
            selected_ = {};
            return;
        }
        if (isPrefabRoot(selected_.path[0])) {
            log("A prefab keeps its root node");
            return;
        }
        if (const auto* node = selectedNode()) {
            const auto& nodes = view_.instances[selected_.instance].expansion.nodes;
            const auto found = references_.find({static_cast<std::size_t>(selected_.instance), node - nodes.data()});
            if (found != references_.end())
                for (const auto& referrer : found->second)
                    log("Deleted " + node->node.name + ", which " + referrer.label + " referred to");
        }
        checkpoint();
        fileOf(selected_.instance)->removeNode(selected_.path[0]);
    } else if (selected_.kind == Selection::Kind::System) {
        // One an instance added leaves its overrides; one it brought is removed by them.
        const auto target = systemTarget();
        if (!target.system || target.addedTo) {
            if (!target.addedTo && !target.overrides) return;
            checkpoint();
            if (target.addedTo) {
                auto& added = target.addedTo->addedSystems;
                added.erase(std::remove_if(added.begin(), added.end(), [&](const System& each) { return each.id == selected_.system; }),
                            added.end());
            } else {
                target.overrides->removedSystems.push_back({target.instance, selected_.system});
            }
            selected_ = {};
            return;
        }
        checkpoint();
        (selected_.instance < 0 ? static_cast<SystemSet&>(doc_.world.level) : *fileOf(selected_.instance)).removeSystem(selected_.system);
    } else {
        return;
    }
    selected_ = {};
}

bool Editor::openLevel(const std::string& path) {
    if(!project_ || runtime_) return false;
    std::snprintf(levelPath_,sizeof levelPath_,"%s",path.c_str());
    return open();
}

bool Editor::open() {
    try {
        Document loaded;
        loaded.gameRendering=doc_.gameRendering;
        loaded.world.level = Level::load(project_->resolve(levelPath_));
        if(!loaded.world.level.mixer.empty()) {
            try { loaded.mixers.emplace(loaded.world.level.mixer,MixerDefinition::load(project_->resolve(loaded.world.level.mixer))); }
            catch(const std::exception& e) { log(e.what()); }
        }
        for (const auto& instance : loaded.world.level.scenes) {
            if (loaded.world.scenes.count(instance.scene)) continue;
            const auto file = project_->resolve(instance.scene);
            if (std::filesystem::exists(file)) {
                loaded.world.scenes[instance.scene] = Scene::load(file);
            } else {
                log("Scene " + instance.scene + " does not exist yet; it will be created on save");
                loaded.world.scenes[instance.scene] = Scene();
            }
        }
        checkpoint();
        doc_ = std::move(loaded);
        contexts_.resize(1);
        navigate_.reset();
        dirty_ = doc_.gameRendering.vsync!=project_->rendering.vsync;
        selected_ = {};
        areaLightModel_=-1;
        refreshView();
        if (!view_.instances.empty() && !view_.instances[0].expansion.nodes.empty())
            selected_ = {Selection::Kind::Node, 0, view_.instances[0].expansion.nodes[0].path};
        focusSelection();
        // The environment and GI apply from the scene shown at the next lighting update.
        gi_->clear();
        giScene_.clear();
        giEnabled_=false;
        lightingScene_.reset();
        showGrid_=doc_.world.level.rendering.grid;
        log(std::string("Opened ") + levelPath_);
        return true;
    } catch (const std::exception& error) {
        fileError_ = error.what();
        log(std::string("Open failed: ") + fileError_);
        return false;
    }
}

bool Editor::save() {
    try {
        for (const auto& [scenePath, scene] : doc_.world.scenes) scene.save(project_->resolve(scenePath));
        for (const auto& [prefabPath, prefab] : doc_.prefabs) prefab.save(project_->resolve(prefabPath));
        for(const auto& [path,mixer]:doc_.mixers) mixer.save(project_->resolve(path));
        doc_.world.level.save(project_->resolve(levelPath_));
        auto project=*project_;
        project.rendering=doc_.gameRendering;
        project.save();
        project_=std::move(project);
        dirty_ = false;
        log(std::string("Saved ") + levelPath_ + ", " + std::to_string(doc_.world.scenes.size()) + " scene(s) and " +
            std::to_string(doc_.prefabs.size()) + " prefab(s)");
        return true;
    } catch (const std::exception& error) {
        fileError_ = error.what();
        log(std::string("Save failed: ") + fileError_);
        return false;
    }
}

void Editor::shortcuts() {
    if (ImGui::IsKeyPressed(ImGuiKey_Pause, false)) togglePause();
    if (ImGui::GetIO().WantTextInput || ImGui::IsMouseDown(ImGuiMouseButton_Right)) return;
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) runtime_ ? stop() : play();
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) focusSelection();
    // Play mode runs on a copy, so editing is disabled until Stop.
    if (runtime_) return;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
        redo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) save();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) showFiles_ = true, saving_ = false;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) duplicate();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) erase();
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmoOperation_ = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmoOperation_ = ImGuizmo::ROTATE;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) gizmoOperation_ = ImGuizmo::SCALE;
}

void Editor::menu() {
    if (!ImGui::BeginMainMenuBar()) return;
    const bool editing = !runtime_;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Level", nullptr, false, editing)) {
            checkpoint();
            doc_.world=World();
            doc_.prefabs.clear();
            contexts_.resize(1);
            navigate_.reset();
            selected_ = {};
            std::snprintf(levelPath_, sizeof levelPath_, "levels/untitled.level.lton");
        }
        if (ImGui::MenuItem("Open...", "Ctrl+O", false, editing)) showFiles_ = true, saving_ = false;
        if (ImGui::MenuItem("Save", "Ctrl+S", false, editing)) save();
        if (ImGui::MenuItem("Save As...", nullptr, false, editing)) showFiles_ = true, saving_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Close Project")) requestLeave(Leave::CloseProject);
        if (ImGui::MenuItem("Exit")) requestLeave(Leave::Exit);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit", editing)) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo_.empty())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redo_.empty())) redo();
        ImGui::Separator();
        if (ImGui::BeginMenu("Add Primitive")) {
            if (ImGui::MenuItem("Box")) addNode(PrimitiveKind::Box);
            if (ImGui::MenuItem("Sphere")) addNode(PrimitiveKind::Sphere);
            if (ImGui::MenuItem("Capsule")) addNode(PrimitiveKind::Capsule);
            if (ImGui::MenuItem("Plane")) addNode(PrimitiveKind::Plane);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Add System")) addSystem();
        if (ImGui::MenuItem("Duplicate Node", "Ctrl+D", false, targetOf(selected_.instance, selected_.path).node))
            duplicate();
        if (ImGui::MenuItem("Delete", "Del", false, selectedNode() || selectedSystem())) erase();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Grid", nullptr, &showGrid_);
        ImGui::MenuItem("Statistics", nullptr, &showStatistics_);
        bool skybox = skyboxEnabled_;
        if (ImGui::MenuItem("Skybox", nullptr, &skybox)) setSkyboxEnabled(skybox);
        bool probes = gi_->debugVisible();
        if (ImGui::MenuItem("GI Probes", nullptr, &probes)) gi_->setDebugVisible(probes);
        bool shadows=shadowsEnabled_;
        if(ImGui::MenuItem("Shadows",nullptr,&shadows)) setShadowsEnabled(shadows);
        ImGui::MenuItem("Motion Blur (Play)",nullptr,&motionBlurInPlay_);
        ImGui::MenuItem("Ambient Occlusion",nullptr,&ambientOcclusionEnabled_);
        ImGui::MenuItem("Screen-Space Reflections",nullptr,&reflectionsEnabled_);
        ImGui::MenuItem("Subsurface Scattering",nullptr,&subsurfaceEnabled_);
        ImGui::MenuItem("Orthographic", nullptr, &orthographic_);
        if (ImGui::MenuItem("Focus Selection", "F")) focusSelection();
        ImGui::EndMenu();
    }
    if(ImGui::BeginMenu("Lighting")) {
        auto& r=doc_.world.level.rendering;
        if(ImGui::MenuItem("Forward Clustered",nullptr,r.clustered)) { checkpoint(); r.clustered=true; }
        if(ImGui::MenuItem("Forward3D",nullptr,!r.clustered)) { checkpoint(); r.clustered=false; }
        bool debug=r.debugClusters;
        if(ImGui::MenuItem("Show Clusters",nullptr,&debug)) { checkpoint(); r.debugClusters=debug; }
        ImGui::Separator();
        if(ImGui::MenuItem("Area Lights: Authored",nullptr,areaLightModel_<0)) areaLightModel_=-1;
        if(ImGui::MenuItem("Area Lights: Approx (textured)",nullptr,areaLightModel_==0)) areaLightModel_=0;
        if(ImGui::MenuItem("Area Lights: LTC",nullptr,areaLightModel_==1)) areaLightModel_=1;
        ImGui::Separator();
        ImGui::BeginDisabled(runtime_!=nullptr);
        // The environment and GI are the shown scene's, set in its Inspector (TDDja/13).
        if(ImGui::MenuItem("Scene Lighting...",nullptr,false,giInstance()>=0))
            selected_=content() ? Selection{Selection::Kind::Level} : Selection{Selection::Kind::Scene,0};
        if(ImGui::MenuItem("Bake Global Illumination",nullptr,false,lighting().gi.enabled && giInstance()>=0))
            requestGlobalIlluminationBake();
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }
    if(ImGui::BeginMenu("Particles")) {
        ImGui::BeginDisabled(runtime_!=nullptr);
        ImGui::MenuItem("Preview in Edit Mode",nullptr,&particlePreview_);
        ImGui::EndDisabled();
        if(ImGui::MenuItem("Alpha Blending",nullptr,particleBlending_==0)) particleBlending_=0;
        if(ImGui::MenuItem("Alpha Hashing",nullptr,particleBlending_==1)) particleBlending_=1;
        if(ImGui::MenuItem("Restart")) clearParticles();
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem(runtime_ ? "Stop" : "Play", "F5")) runtime_ ? stop() : play();
    if (ImGui::MenuItem(paused() ? "Resume" : "Pause", "Pause", paused(), runtime_ != nullptr)) togglePause();
    if (runtime_) ImGui::TextColored(paused() ? ImVec4(1.0f, 0.8f, 0.3f, 1) : ImVec4(0.4f, 0.9f, 0.5f, 1),
                                   paused() ? "Paused" : "Playing");
    ImGui::TextDisabled("%s%s", levelPath_, dirty_ ? " *" : "");
    ImGui::EndMainMenuBar();
}

void Editor::fileDialog() {
    if (showFiles_) {
        ImGui::OpenPopup("Level File");
        showFiles_ = false;
        fileError_.clear();
    }
    if (!ImGui::BeginPopupModal("Level File", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::SetNextItemWidth(420);
    const bool enter = ImGui::InputText("Path", levelPath_, sizeof levelPath_,
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    if (!fileError_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", fileError_.c_str());
    if ((ImGui::Button(saving_ ? "Save" : "Open") || enter) && (saving_ ? save() : open()))
        ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Editor::requestLeave(Leave action) {
    leaving_ = action;
    if (dirty_) {
        if (runtime_) stop();
        confirmClose_ = true;
    }
    else leave(action);
}

void Editor::leave(Leave action) {
    if (action == Leave::Exit) {
        Ogre::LogManager::getSingleton().logMessage("Shutdown: exit accepted");
        close_ = true;
    }
    else closeProject();
}

void Editor::closeDialog() {
    if (confirmClose_) {
        ImGui::OpenPopup("Unsaved Changes");
        confirmClose_ = false;
        fileError_.clear();
    }
    if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("Save changes to %s first?", levelPath_);
    if (!fileError_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", fileError_.c_str());
    // Close the popup first, because leaving may replace what it was drawn for.
    if (ImGui::Button("Save") && save()) ImGui::CloseCurrentPopup(), leave(leaving_);
    ImGui::SameLine();
    if (ImGui::Button("Discard")) ImGui::CloseCurrentPopup(), leave(leaving_);
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Editor::viewport() {
    bool visible = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin("Viewport", nullptr,
              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const ImVec2 size = ImGui::GetContentRegionAvail();
        if (size.x >= 1 && size.y >= 1) {
            visible = true;
            viewportVisible_=true;
            resizeViewport(static_cast<unsigned>(size.x), static_cast<unsigned>(size.y));
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Image(texture_, size);
            // ImGuizmo will not start a drag while another item was hovered in the previous
            // frame, so the camera/picking input steps aside while the gizmo is under the mouse,
            // Navigation can start over a gizmo, and owns the gesture until release.
            const bool startNavigation = ImGui::IsWindowHovered() &&
                (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle));
            if (viewDrag_ != ViewDrag::None || (!ImGuizmo::IsUsing() && (!gizmoOver_ || startNavigation))) {
                ImGui::SetCursorScreenPos(origin);
                ImGui::SetNextItemAllowOverlap();
                ImGui::InvisibleButton("ViewportInput", size, ImGuiButtonFlags_MouseButtonLeft |
                                                                  ImGuiButtonFlags_MouseButtonRight |
                                                                  ImGuiButtonFlags_MouseButtonMiddle);
                const auto& io = ImGui::GetIO();
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    pick((io.MousePos.x - origin.x) / size.x, (io.MousePos.y - origin.y) / size.y);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) viewDrag_ = ViewDrag::Fly;
                if (ImGui::IsItemClicked(ImGuiMouseButton_Middle))
                    viewDrag_ = io.KeyAlt ? (io.KeyCtrl ? ViewDrag::Dolly : ViewDrag::Orbit) : ViewDrag::Pan;
                if (io.MouseWheel != 0) {
                    if (viewDrag_ == ViewDrag::Fly && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right))
                        flySpeed_ = std::clamp(flySpeed_ * std::pow(1.25f, io.MouseWheel), 0.1f, 500.0f);
                    else if (ImGui::IsItemHovered() && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
                        distance_ = std::clamp(distance_ * std::pow(0.88f, io.MouseWheel), 0.5f, 5000.0f);
                }
                const auto offset = [&] {
                    return Ogre::Vector3(std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_),
                                         std::cos(pitch_) * std::cos(yaw_));
                };
                const bool flying = viewDrag_ == ViewDrag::Fly;
                const auto button = flying ? ImGuiMouseButton_Right : ImGuiMouseButton_Middle;
                if (viewDrag_ != ViewDrag::None && ImGui::IsItemActive() && ImGui::IsMouseDown(button)) {
                    if (flying || viewDrag_ == ViewDrag::Orbit) {
                        const auto position = target_ + offset() * distance_;
                        yaw_ -= io.MouseDelta.x * 0.01f;
                        pitch_ = std::clamp(pitch_ + io.MouseDelta.y * 0.01f, -1.5f, 1.5f);
                        if (flying) target_ = position - offset() * distance_;
                    }
                    if (flying && !io.WantTextInput) {
                        const auto forward = -offset();
                        const Ogre::Vector3 right(std::cos(yaw_), 0, -std::sin(yaw_));
                        auto move = forward * float(ImGui::IsKeyDown(ImGuiKey_W) - ImGui::IsKeyDown(ImGuiKey_S)) +
                                    right * float(ImGui::IsKeyDown(ImGuiKey_D) - ImGui::IsKeyDown(ImGuiKey_A)) +
                                    Ogre::Vector3::UNIT_Y * float(ImGui::IsKeyDown(ImGuiKey_E) - ImGui::IsKeyDown(ImGuiKey_Q));
                        if (move.squaredLength() > 0) {
                            move.normalise();
                            const float speed = flySpeed_ * (ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 2.0f : 1.0f);
                            target_ += move * (speed * io.DeltaTime);
                        }
                    }
                    if (viewDrag_ == ViewDrag::Pan && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
                        target_ += (camera_.getUp() * io.MouseDelta.y - camera_.getRight() * io.MouseDelta.x) *
                                   (distance_ * 0.0015f);
                    if (viewDrag_ == ViewDrag::Dolly)
                        distance_ = std::clamp(distance_ * std::exp(io.MouseDelta.y * 0.01f), 0.5f, 5000.0f);
                }
                if (viewDrag_ == ViewDrag::Pan && ImGui::IsMouseReleased(ImGuiMouseButton_Middle) &&
                    ImGui::IsItemHovered() && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Middle))
                    focusAt((io.MousePos.x - origin.x) / size.x, (io.MousePos.y - origin.y) / size.y);
                if (!ImGui::IsMouseDown(button) || !ImGui::IsItemActive()) viewDrag_ = ViewDrag::None;
            }
            gizmo(origin, size);
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 6, origin.y + 6));
            ImGui::RadioButton("Move (W)", &gizmoOperation_, ImGuizmo::TRANSLATE);
            ImGui::SameLine();
            ImGui::RadioButton("Rotate (E)", &gizmoOperation_, ImGuizmo::ROTATE);
            ImGui::SameLine();
            ImGui::RadioButton("Scale (R)", &gizmoOperation_, ImGuizmo::SCALE);
            ImGui::SameLine();
            ImGui::Checkbox("Local", &gizmoLocal_);
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 6, origin.y + 30));
            breadcrumb();
            if (showStatistics_) {
                const float fontScale = ImGui::GetFontSize() / 13.0f;
                const ImVec2 statisticsSize(280 * fontScale, (statisticsCollapsed_ ? 28 : 200) * fontScale);
                ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x - statisticsSize.x - 10, origin.y + 8));
                ImGui::SetNextWindowBgAlpha(0.75f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
                if (ImGui::BeginChild("ViewportStatistics", statisticsSize, ImGuiChildFlags_Borders,
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar)) {
                    if (ImGui::SmallButton(statisticsCollapsed_ ? "+" : "-")) statisticsCollapsed_ = !statisticsCollapsed_;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(statisticsCollapsed_ ? "Expand statistics" : "Minimize statistics");
                    ImGui::SameLine();
                    bool vsync=preferences_.vsync;
                    if(ImGui::Checkbox("VSync",&vsync)) setEditorVSync(vsync);
                    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Editor window VSync. Game rendering has a separate setting in OGRE.");
                    ImGui::SameLine();
                    const auto fps = ImGui::GetIO().Framerate;
                    ImGui::Text("FPS %.1f | %.2f ms", fps, fps > 0 ? 1000.0f / fps : 0.0f);
                    if (!statisticsCollapsed_) {
                        const auto& t = averageTimings_;
                        ImGui::Text("Nodes %zu | Bodies %zu", renderObjects_.size(), physics_ ? physics_->bodyCount() : 0);
                        ImGui::Text("Particles ~%zu | Systems %zu",particleCount_,particleSystemCount_);
                        ImGui::Text("UI / scripts: %.2f / %.2f ms", t.uiMs, t.scriptMs);
                        ImGui::Text("Physics: %.2f ms", t.physicsMs);
                        ImGui::Text("Scene snapshot: %.2f ms", t.snapshotMs);
                        ImGui::Text("Render sync: %.2f ms", t.renderSyncMs);
                        ImGui::Text("Render / present: %.2f ms", t.renderMs);
                        ImGui::Text("Simulation wait: %.2f ms", t.waitMs);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("CPU wall times; with frame delay, simulation and rendering overlap.\nRender / present includes GPU and display waits.");
                    }
                }
                ImGui::EndChild();
                ImGui::PopStyleVar();
            }
        }
    }
    if (!visible) viewDrag_ = ViewDrag::None;
    ImGui::End();
    ImGui::PopStyleVar();
}

void Editor::browser() {
    if (ImGui::Begin("Browser")) {
        if (ImGui::BeginTabBar("BrowserTabs")) {
            if (ImGui::BeginTabItem("Levels")) {
                constexpr std::string_view suffix = ".level.lton";
                std::error_code error;
                for (const auto& entry : std::filesystem::directory_iterator(project_->resolve("levels"), error)) {
                    const auto file = entry.path().filename().u8string();
                    if (file.size() <= suffix.size() ||
                        file.compare(file.size() - suffix.size(), suffix.size(), suffix) != 0)
                        continue;
                    const auto path = "levels/" + file;
                    if (ImGui::Selectable(file.c_str(), path == levelPath_) && !runtime_ &&
                        path.size() < sizeof levelPath_) {
                        std::memcpy(levelPath_, path.c_str(), path.size() + 1);
                        open();
                    }
                }
                ImGui::EndTabItem();
            }
            // One click picks the file for Add Scene / Add Instance; a double click opens it.
            const auto files = [&](const char* tab, const char* folder, std::string_view suffix, char* pick,
                                   std::size_t capacity, Context::Kind kind) {
                if (!ImGui::BeginTabItem(tab)) return;
                std::error_code error;
                for (const auto& entry : std::filesystem::directory_iterator(project_->resolve(folder), error)) {
                    const auto file = entry.path().filename().u8string();
                    if (file.size() <= suffix.size() ||
                        file.compare(file.size() - suffix.size(), suffix.size(), suffix) != 0)
                        continue;
                    const auto relative = std::string(folder) + "/" + file;
                    const bool variant = kind == Context::Kind::Prefab && isVariant(relative);
                    if (variant) ImGui::PushStyleColor(ImGuiCol_Text, variantColour);
                    if (ImGui::Selectable(file.c_str(), relative == pick, ImGuiSelectableFlags_AllowDoubleClick) &&
                        relative.size() < capacity) {
                        std::memcpy(pick, relative.c_str(), relative.size() + 1);
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !runtime_) enter({kind, relative});
                    }
                    // A node dropped on a prefab file replaces it, or applies an instance's overrides to it.
                    if (kind == Context::Kind::Prefab && !runtime_ && ImGui::BeginDragDropTarget()) {
                        if (const auto* payload = ImGui::AcceptDragDropPayload(nodePayload)) {
                            const auto& dragged = *static_cast<const DragNode*>(payload->Data);
                            dropOnPrefab(dragged.instance, dragged.id, relative);
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (variant) ImGui::PopStyleColor();
                }
                // A node dropped anywhere on the Prefabs tab becomes a prefab (TDDja/06 7).
                if (kind == Context::Kind::Prefab && !runtime_ &&
                    ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->InnerRect, ImGui::GetID("prefabify"))) {
                    if (const auto* payload = ImGui::AcceptDragDropPayload(nodePayload)) {
                        const auto& dragged = *static_cast<const DragNode*>(payload->Data);
                        prefabify(dragged.instance, dragged.id);
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::EndTabItem();
            };
            files("Scenes", "scenes", ".scene.lton", newScenePath_, sizeof newScenePath_, Context::Kind::Scene);
            files("Prefabs", "prefabs", ".prefab.lton", newPrefabPath_, sizeof newPrefabPath_, Context::Kind::Prefab);
            audioBrowser();
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void Editor::console() {
    if (ImGui::Begin("Console")) {
        // The log fills the panel above one line for the prompt.
        if (ImGui::BeginChild("##messages", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()))) {
            for (const auto& message : messages_) ImGui::TextUnformatted(message.c_str());
            // Follows new lines while scrolled to the bottom.
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        auto* on = consoleRuntime();
        ImGui::TextUnformatted(consoleMore_ && consoleMore_ == on ? "." : ">");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        // Up and Down go through the lines run before.
        const auto recall = [](ImGuiInputTextCallbackData* data) {
            auto& self = *static_cast<Editor*>(data->UserData);
            auto& history = self.consoleHistory_;
            if (history.empty()) return 0;
            auto& at = self.consoleRecall_;
            if (data->EventKey == ImGuiKey_UpArrow && at > 0) --at;
            else if (data->EventKey == ImGuiKey_DownArrow && at < history.size()) ++at;
            data->DeleteChars(0, data->BufTextLen);
            if (at < history.size()) data->InsertChars(0, history[at].c_str());
            return 0;
        };
        if (ImGui::InputText("##prompt", consoleInput_, sizeof consoleInput_,
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, recall, this)) {
            evaluate(consoleInput_);
            consoleInput_[0] = '\0';
            ImGui::SetKeyboardFocusHere(-1);  // Stays in the prompt for the next line.
        }
        // Escape leaves an input that will not be finished.
        if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape) && consoleMore_) {
            consoleMore_->dropInput();
            consoleMore_ = nullptr;
        }
    }
    ImGui::End();
}

ScriptRuntime* Editor::consoleRuntime() {
    if (runtime_) return runtime_.get();
    if (!consoleIdle_ && project_) {
        try {
            consoleIdle_ = std::make_unique<ScriptRuntime>(consoleWorld_, consoleView_, project_->root,
                                                           [this](const std::string& line) { log(line); }, PlayScope::Level);
        } catch (const ScriptError& error) {
            log(error.what());
        }
    }
    return consoleIdle_.get();
}

void Editor::evaluate(const std::string& line) {
    auto* on = consoleRuntime();
    if (!on) return;
    const bool continuing = consoleMore_ == on;
    log((continuing ? ". " : "> ") + line);
    if (!line.empty() && (consoleHistory_.empty() || consoleHistory_.back() != line)) consoleHistory_.push_back(line);
    consoleRecall_ = consoleHistory_.size();
    const auto answer = on->evaluate(line);
    consoleMore_ = answer.more ? on : nullptr;
    if (!answer.text.empty()) log(answer.text);
}

void Editor::gizmo(ImVec2 origin, ImVec2 size) {
    const auto* node = selectedNode();
    auto* placed = selectedScene();
    Ogre::SceneNode* moved = placed && selected_.instance < static_cast<int>(sceneRoots_.size())
                                 ? sceneRoots_[selected_.instance]
                                 : nullptr;
    for (const auto& each : renderObjects_)
        if (node && selected_.instance == static_cast<int>(each.instance) &&
            view_.instances[each.instance].expansion.nodes[each.index].path == selected_.path)
            moved = each.node;
    // Only a node with a Transform can be moved.
    if (!moved || runtime_ || (node && !node->node.transform)) {
        gizmoOver_ = false;
        return;
    }
    float view[16], projection[16], model[16];
    toColumnMajor(camera_.getViewMatrix(), view);
    toColumnMajor(camera_.getProjectionMatrix(), projection);
    toColumnMajor(moved->_getFullTransformUpdated(), model);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
    ImGuizmo::SetOrthographic(orthographic_);
    // Navigation draws the gizmo without letting it highlight or take the mouse.
    ImGuizmo::Enable(viewDrag_ == ViewDrag::None);
    const bool changed =
        ImGuizmo::Manipulate(view, projection, static_cast<ImGuizmo::OPERATION>(gizmoOperation_),
                             gizmoLocal_ ? ImGuizmo::LOCAL : ImGuizmo::WORLD, model);
    // One undo step per drag.
    const bool using_ = ImGuizmo::IsUsing();
    if (using_ && !gizmoUsing_) editStart_ = doc_;
    if (!using_ && gizmoUsing_) commit(std::move(editStart_));
    gizmoUsing_ = using_;
    gizmoOver_ = ImGuizmo::IsOver();
    if (!changed) return;

    // The gizmo works in world space; the node stores its transform relative to its parent.
    auto* parent = moved->getParentSceneNode();
    const auto local = parent->_getFullTransformUpdated().inverseAffine() * fromColumnMajor(model);
    Ogre::Vector3 position, scale;
    Ogre::Quaternion rotation;
    local.decomposition(position, scale, rotation);
    Ogre::Matrix3 axes;
    rotation.ToRotationMatrix(axes);
    Ogre::Radian z, y, x;
    axes.ToEulerAnglesZYX(z, y, x);
    Transform result{fromOgre(position), {x.valueDegrees(), y.valueDegrees(), z.valueDegrees()}, fromOgre(scale)};
    for (float* component : {&result.scale.x, &result.scale.y, &result.scale.z}) *component = std::max(*component, 0.001f);
    if (placed) {
        placed->transform = result;
        return;
    }
    auto value = node->node;
    value.transform = result;
    writeNode(value, 2 | 4 | 8);
}

void Editor::updateCamera() {
    const Ogre::Vector3 offset(std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_),
                               std::cos(pitch_) * std::cos(yaw_));
    camera_.setPosition(target_ + offset * distance_);
    camera_.lookAt(target_);
    camera_.setFOVy(Ogre::Degree(viewFieldOfView_));
    camera_.setNearClipDistance(viewNear_);
    camera_.setFarClipDistance(viewFar_);
    camera_.setAspectRatio(static_cast<Ogre::Real>(viewportWidth_) / viewportHeight_);
    camera_.setProjectionType(orthographic_ ? Ogre::PT_ORTHOGRAPHIC : Ogre::PT_PERSPECTIVE);
    if (orthographic_)
        camera_.setOrthoWindowHeight(2 * distance_ * std::tan(camera_.getFOVy().valueRadians() / 2));
}

void Editor::alignViewToCamera(std::size_t instance, std::size_t index, const CameraComponent& camera, bool lookThrough) {
    if (instance >= sceneNodes_.size() || index >= sceneNodes_[instance].size()) return;
    auto* node = sceneNodes_[instance][index];
    // A cut: the history shows another view.
    taaReset_ = true;
    historyReady_ = false;
    // The camera looks along the node's -Z; the view keeps its distance to what it orbits.
    const auto position = node->_getDerivedPositionUpdated();
    const auto forward = node->_getDerivedOrientationUpdated() * Ogre::Vector3::NEGATIVE_UNIT_Z;
    pitch_ = std::clamp(std::asin(std::clamp(-forward.y, -1.0f, 1.0f)), -1.5f, 1.5f);
    yaw_ = std::atan2(-forward.x, -forward.z);
    const Ogre::Vector3 offset(std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_), std::cos(pitch_) * std::cos(yaw_));
    target_ = position - offset * distance_;
    viewFieldOfView_ = camera.fieldOfView;
    // The view keeps its own clip distances, depth of field and anti-aliasing unless it is to
    // show exactly what the camera does.
    if (lookThrough) {
        viewNear_ = camera.nearClip;
        viewFar_ = camera.farClip;
        viewFocusDistance_ = camera.focusDistance;
        viewFStop_ = camera.fStop;
        viewAntiAliasing_ = camera.antiAliasing;
    }
    updateCamera();
}

void Editor::alignCameraToView() {
    const auto* expanded = selectedNode();
    if (!expanded || !expanded->node.camera || selected_.instance < 0 ||
        std::size_t(selected_.instance) >= sceneNodes_.size())
        return;
    const auto& created = sceneNodes_[std::size_t(selected_.instance)];
    std::size_t index = 0;
    while (index < created.size() && view_.instances[std::size_t(selected_.instance)].expansion.nodes[index].path != selected_.path)
        ++index;
    if (index == created.size()) return;
    // The view in world space, into the node's parent's space, keeping the node's scale.
    auto* node = created[index];
    Ogre::Matrix4 world;
    world.makeTransform(camera_.getPosition(), node->_getDerivedScaleUpdated(), camera_.getOrientation());
    const auto local = node->getParentSceneNode()->_getFullTransformUpdated().inverseAffine() * world;
    Ogre::Vector3 position, scale;
    Ogre::Quaternion rotation;
    local.decomposition(position, scale, rotation);
    Ogre::Matrix3 axes;
    rotation.ToRotationMatrix(axes);
    Ogre::Radian z, y, x;
    axes.ToEulerAnglesZYX(z, y, x);
    checkpoint();
    auto value = expanded->node;
    auto& transform = value.transform ? *value.transform : value.transform.emplace();
    transform.position = fromOgre(position);
    transform.rotation = {x.valueDegrees(), y.valueDegrees(), z.valueDegrees()};
    writeNode(value, 2 | 4);
    CameraComponent camera = *expanded->node.camera;
    camera.fieldOfView = viewFieldOfView_;
    const auto target = targetOf(selected_.instance, selected_.path);
    if (target.node && target.node->camera) target.node->camera = camera;
    else if (target.overrides) target.overrides->node(target.relative).camera = camera;
}

bool Editor::viewFromFirstCamera() {
    for (std::size_t i = 0; i < view_.instances.size(); ++i)
        for (std::size_t k = 0; k < view_.instances[i].expansion.nodes.size(); ++k)
            if (const auto& camera = view_.instances[i].expansion.nodes[k].node.camera) {
                // As the camera sees it: nothing selected, so no outline or gizmo either.
                selected_ = {};
                alignViewToCamera(i, k, *camera, true);
                return true;
            }
    return false;
}

void Editor::focusSelection() {
    if (selectedScene() && selected_.instance < static_cast<int>(sceneRoots_.size())) {
        target_ = sceneRoots_[selected_.instance]->_getDerivedPositionUpdated();
        return;
    }
    for (const auto& render : renderObjects_) {
        if (selected_.kind != Selection::Kind::Node || selected_.instance != static_cast<int>(render.instance) ||
            view_.instances[render.instance].expansion.nodes[render.index].path != selected_.path)
            continue;
        target_ = render.node->_getFullTransformUpdated().transformAffine(toOgre((render.lower+render.upper)*.5f));
        const auto size = render.node->_getDerivedScaleUpdated()*toOgre(render.upper-render.lower);
        distance_ = std::max({Ogre::Real(3), size.x * 1.6f, size.y * 1.6f, size.z * 1.6f});
    }
}

void Editor::pick(float x, float y) {
    const Ogre::Ray ray = camera_.getCameraToViewportRay(x, y);
    auto nearest = std::numeric_limits<Ogre::Real>::max();
    selected_ = {};
    for (const auto& render : renderObjects_) {
        if (render.shape.kind==PrimitiveKind::None && !render.placedModel.root) continue;
        auto lower=render.lower, upper=render.upper;
        if(upper.y-lower.y<.02f) lower.y-=.01f, upper.y+=.01f;
        const Ogre::AxisAlignedBox bounds(toOgre(lower),toOgre(upper));
        // Test in node space; the unnormalized direction keeps distances comparable.
        const auto inverse = render.node->_getFullTransformUpdated().inverseAffine();
        const Ogre::Ray local(inverse.transformAffine(ray.getOrigin()),
                              inverse.transformDirectionAffine(ray.getDirection()));
        const auto hit = local.intersects(bounds);
        if (hit.first && hit.second < nearest) {
            nearest = hit.second;
            selected_ = {Selection::Kind::Node, static_cast<int>(render.instance),
                         view_.instances[render.instance].expansion.nodes[render.index].path};
        }
    }
}

void Editor::focusAt(float x, float y) {
    const auto ray = camera_.getCameraToViewportRay(x, y);
    auto nearest = std::numeric_limits<Ogre::Real>::max();
    for (const auto& render : renderObjects_) {
        const auto inverse = render.node->_getFullTransformUpdated().inverseAffine();
        // Keep the direction unnormalized so t remains a world-ray distance under scaling.
        const Ogre::Ray local(inverse.transformAffine(ray.getOrigin()),
                              inverse.transformDirectionAffine(ray.getDirection()));
        const auto geometry = primitiveMesh(render.shape);
        for (std::size_t i = 0; i < geometry.indices.size(); i += 3) {
            const auto hit = Ogre::Math::intersects(local,
                toOgre(geometry.vertices[geometry.indices[i]].position),
                toOgre(geometry.vertices[geometry.indices[i + 1]].position),
                toOgre(geometry.vertices[geometry.indices[i + 2]].position), true, true);
            if (hit.first && hit.second > 0 && hit.second < nearest) nearest = hit.second;
        }
    }
    if (nearest == std::numeric_limits<Ogre::Real>::max()) return;
    const auto target = ray.getPoint(nearest);
    const auto offset = camera_.getPosition() - target;
    const auto distance = offset.length();
    if (distance < 0.001f) return;
    target_ = target;
    distance_ = distance;
    yaw_ = std::atan2(offset.x, offset.z);
    pitch_ = std::asin(std::clamp(offset.y / distance, -1.0f, 1.0f));
}

void Editor::resizeViewport(unsigned width, unsigned height) {
    if (width == viewportWidth_ && height == viewportHeight_) return;
    destroyWorkspaces();
    // Resizing in place keeps the TextureGpu pointer that ImGui caches its material by.
    texture_->scheduleTransitionTo(Ogre::GpuResidency::OnStorage);
    texture_->setResolution(width, height);
    texture_->scheduleTransitionTo(Ogre::GpuResidency::Resident);
    viewportWidth_ = width;
    viewportHeight_ = height;
    createWorkspaces();
    taaReset_ = true;
    historyReady_ = false;
}

void Editor::createWorkspaces() {
    auto* compositor = root_.getCompositorManager2();
    viewportWorkspace_ = compositor->addWorkspace(&scene_, texture_, &camera_, viewportWorkspaceName, project_.has_value() && viewportVisible_);
    ambientOcclusion_->texture = viewportWorkspace_->getNodeSequence()[0]->getDefinedTexture("Ssao");
    const Ogre::CompositorChannelVec channels{window_.getTexture(), texture_};
    windowWorkspace_ = compositor->addWorkspace(&scene_, channels, &camera_, windowWorkspaceName, true);
}

void Editor::destroyWorkspaces() {
    auto* compositor = root_.getCompositorManager2();
    if (ambientOcclusion_) ambientOcclusion_->texture = nullptr;
    for (auto** workspace : {&windowWorkspace_, &viewportWorkspace_}) {
        if (*workspace) compositor->removeWorkspace(*workspace);
        *workspace = nullptr;
    }
}

void Editor::log(const std::string& message) {
    std::lock_guard<std::mutex> lock(messagesMutex_);
    messages_.push_back(message);
    if (messages_.size() > logLimit) messages_.erase(messages_.begin());
    Ogre::LogManager::getSingleton().logMessage("Editor: " + message);
}
}
