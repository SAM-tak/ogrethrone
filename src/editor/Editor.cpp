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
#include <OgreForwardPlusBase.h>
#include <OgreTextureFilters.h>
#include <OgreTextureGpu.h>
#include <OgreAtmosphereNpr.h>
#include <OgreMaterialManager.h>
#include <OgreMaterial.h>
#include <OgreTechnique.h>
#include <OgrePass.h>
#include <OgreResourceGroupManager.h>
#include <OgreHlmsPbs.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreHlmsSamplerblock.h>
#include <Compositor/Pass/OgreCompositorPassDef.h>
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
#include <OgreRay.h>
#include <OgreRenderSystem.h>
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

/// Creates a three-cascade depth atlas for the default directional light.
void createShadowNode(Ogre::Root& root) {
    Ogre::ShadowNodeHelper::ShadowParam sun{};
    sun.technique=Ogre::SHADOWMAP_PSSM;
    sun.numPssmSplits=3;
    sun.addLightType(Ogre::Light::LT_DIRECTIONAL);
    sun.resolution[0]=Ogre::ShadowNodeHelper::Resolution(2048,2048);
    sun.resolution[1]=sun.resolution[2]=Ogre::ShadowNodeHelper::Resolution(1024,1024);
    sun.atlasStart[0]=Ogre::ShadowNodeHelper::Resolution(0,0);
    sun.atlasStart[1]=Ogre::ShadowNodeHelper::Resolution(0,2048);
    sun.atlasStart[2]=Ogre::ShadowNodeHelper::Resolution(1024,2048);
    Ogre::ShadowNodeHelper::ShadowParamVec maps;
    maps.push_back(sun);
    Ogre::ShadowNodeHelper::createShadowNodeWithSettings(root.getCompositorManager2(),
        root.getRenderSystem()->getCapabilities(),"ThroneEditor/SunShadows",maps,false,
        1024,0.7f,1.0f,0.125f,0.2f,2);
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
/// layout that knows the current panels, it docks Scene above Runners on the left,
/// Inspector on the right, Browser and Console below and Viewport in the middle.
void dockSpace() {
    const auto* viewport = ImGui::GetMainViewport();
    const ImGuiID id = ImGui::DockSpaceOverViewport(0, viewport);
    if (const auto* node = ImGui::DockBuilderGetNode(id);
        node && node->IsSplitNode() && ImGui::FindWindowSettingsByID(ImHashStr("Runners"))) {
        // Console came later: in a layout saved before it, it joins Browser's tabs.
        if (!ImGui::FindWindowSettingsByID(ImHashStr("Console")))
            if (const auto* browser = ImGui::FindWindowSettingsByID(ImHashStr("Browser")); browser && browser->DockId)
                ImGui::DockBuilderDockWindow("Console", browser->DockId);
        return;
    }
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, viewport->WorkSize);
    ImGuiID centre = id;
    const ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.18f, nullptr, &centre);
    const ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.26f, nullptr, &centre);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.27f, nullptr, &centre);
    ImGuiID runners = 0;
    const ImGuiID scene = ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.55f, nullptr, &runners);
    ImGui::DockBuilderDockWindow("Scene", scene);
    ImGui::DockBuilderDockWindow("Runners", runners);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Browser", bottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Viewport", centre);
    ImGui::DockBuilderFinish(id);
}
}

Editor::Editor(Ogre::Root& root, Ogre::Window& window, Ogre::SceneManager& scene, Ogre::Camera& camera,
               const std::filesystem::path& project, std::filesystem::path projectList)
    : root_(root), window_(window), scene_(scene), camera_(camera), unlit_(unlitHlms(root)), pbs_(pbsHlms(root)),
      platform_(window), gui_(std::make_unique<Ogre::ImguiManager>()),
      guiPass_(std::make_unique<Ogre::CompositorPassImguiProvider>(gui_.get())),
      projects_(projectList.empty() ? ProjectList() : ProjectList::load(projectList)),
      projectListFile_(std::move(projectList)) {
    auto& io = ImGui::GetIO();
    // The dock layout is kept beside the project list; without one it is not persisted.
    iniFile_ = projectListFile_.empty() ? std::string() : (projectListFile_.parent_path() / "editor.ini").u8string();
    io.IniFilename = iniFile_.empty() ? nullptr : iniFile_.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    styleColours();
    io.Fonts->AddFontDefault();
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
    for (const auto* script : {"Quad.program", "Atmosphere.material"}) {
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
    // Match the visible sun to the existing directional light without relinking lighting.
    sky_->setSunDir(sun_->getDirection(), std::asin(-sun_->getDirection().y) / Ogre::Math::PI);
    sky_->setSky(&scene_, true);
    createShadowNode(root);
    compositor->createBasicWorkspaceDef(viewportWorkspaceName, Ogre::ColourValue(0.11f, 0.12f, 0.14f),"ThroneEditor/SunShadows");
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
    texture_->setPixelFormat(Ogre::PFG_RGBA8_UNORM_SRGB);
    resizeViewport(640, 360);

    createMaterial(unlit_, lineMaterial)->setColour(Ogre::ColourValue::White);
    grid_ = scene.createManualObject();
    grid_->setCastShadows(false);
    addLines(*grid_, gridLines(), Ogre::ColourValue(0.35f, 0.37f, 0.40f));
    gridNode_ = attach(scene, grid_);
    outline_ = scene.createManualObject();
    outline_->setCastShadows(false);
    addLines(*outline_, cubeEdges(), Ogre::ColourValue(1.0f, 0.75f, 0.2f));
    outlineNode_ = attach(scene, outline_);
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
        ImGui::TextUnformatted("Ogre Throne - Projects");
        ImGui::Separator();
        if (ImGui::Button("New Project")) form_ = Form::New, launcherError_.clear();
        ImGui::SameLine();
        if (ImGui::Button("Import")) form_ = Form::Import, launcherError_.clear();
        if (form_ == Form::New) {
            ImGui::InputText("Name", newName_, sizeof newName_);
            ImGui::InputText("Parent folder", newParent_, sizeof newParent_);
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
            ImGui::InputTextWithHint("Folder", "folder containing project.throne.lton", importPath_,
                                     sizeof importPath_);
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
        physics_=std::make_unique<PhysicsWorld>();
        physics_->setSerial(doc_.world.level.serialExecution);
        physics_->load(view_);
        renderSnapshot_ = view_;
        clearParticles();
        log("Play");
    } catch (const std::exception& error) {
        runtime_.reset();
        physics_.reset();
        log(std::string("Play refused:\n") + error.what());
    }
}

void Editor::stop() {
    paused_ = false;
    if (consoleMore_ == runtime_.get()) consoleMore_ = nullptr;
    runtime_.reset();
    physics_.reset();
    renderSnapshot_.reset();
    clearParticles();
    log("Stop");
}

void Editor::togglePause() {
    if (!runtime_) return;
    paused_ = !paused_;
    log(paused_ ? "Pause" : "Resume");
}

Editor::~Editor() {
    auto& shutdownLog=Ogre::LogManager::getSingleton();
    shutdownLog.logMessage("Shutdown: releasing script runtimes");
    consoleMore_=nullptr;
    consoleIdle_.reset();
    runtime_.reset();
    shutdownLog.logMessage("Shutdown: releasing physics world and workers");
    physics_.reset();
    renderSnapshot_.reset();
    shutdownLog.logMessage("Shutdown: releasing editor rendering resources");
    // ImGui would save to io.IniFilename when its context is destroyed, after iniFile_ is gone.
    auto& io = ImGui::GetIO();
    if (io.IniFilename) ImGui::SaveIniSettingsToDisk(io.IniFilename);
    io.IniFilename = nullptr;
    destroyWorkspaces();
    sky_.reset();
    Ogre::ResourceGroupManager::getSingleton().destroyResourceGroup("ThroneEditorSky");
    root_.getCompositorManager2()->setCompositorPassProvider(nullptr);
    // ImGui materials reference the viewport texture, so release them first.
    guiPass_.reset();
    gui_.reset();
    root_.getRenderSystem()->getTextureGpuManager()->destroyTexture(texture_);
    destroyRenderObjects();
    for (auto* mesh : {grid_, outline_}) scene_.destroyManualObject(mesh);
    for (auto* node : {gridNode_, outlineNode_}) scene_.destroySceneNode(node);
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
    if (input) input();
    if (platform_.takeCloseRequest()) requestLeave(Leave::Exit);
    if (close_) return false;
    ImGui::GetIO().DeltaTime = std::max(deltaSeconds, 1.0e-4f);
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    if (!project_) {
        launcher();
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
    runnersPanel();
    inspector();
    viewport();
    browser();
    console();
    fileDialog();
    closeDialog();
    replaceDialog();
    if(close_) {
        ImGui::EndFrame();
        return false;
    }
    measured.uiMs = milliseconds(frameStart, Clock::now());
    bool running = true;
    std::optional<LevelExpansion> nextSnapshot;
    if (physics_) physics_->setSerial(doc_.world.level.serialExecution);
    auto lastMainEnd = Clock::now();
    const auto engine = [&](const std::string& stage) {
        const auto start = Clock::now();
        // Worker callbacks touch their own timing field only; runStages joins before publication.
        if (stage == "Box3D") {
            if (physics_) physics_->advance(deltaSeconds);
            measured.physicsStepMs = milliseconds(start, Clock::now());
            return;
        }
        if (stage == "Animator") return;
        const auto gap = milliseconds(lastMainEnd, start);
        if (stage == "SyncSimulation") measured.waitMs += gap;
        else if(!paused()) measured.scriptMs += gap;
        if (stage == "ApplyPhysics") {
            if (physics_) physics_->apply(view_);
            measured.physicsApplyMs = milliseconds(start, Clock::now());
        } else if (stage == "SyncRender") {
            if (runtime_) nextSnapshot = view_;
            measured.snapshotMs = milliseconds(start, Clock::now());
            const auto prepare = Clock::now();
            if (physics_) physics_->prepare(view_);
            measured.physicsPrepareMs = milliseconds(prepare, Clock::now());
        } else if (stage == "OGRE") {
            syncTransforms(runtime_ && renderSnapshot_ ? &*renderSnapshot_ : nullptr);
            updateCamera();
            scene_.getParticleSystemManager2()->setCameraPosition(camera_.getDerivedPosition());
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
            measured.renderMs = milliseconds(render, Clock::now());
        } else if (stage == "SyncSimulation") {
            if (nextSnapshot) renderSnapshot_ = std::move(nextSnapshot);
            measured.snapshotMs += milliseconds(start, Clock::now());
        }
        lastMainEnd = Clock::now();
        // FetchInput has already collected this frame's OS/ImGui input above.
        // Animator has no bindings until animation components exist.
    };
    try {
        if (paused()) engine("OGRE");
        else if (runtime_) runtime_->update(deltaSeconds, engine);
        else runStages(ScriptRuntime::schedule(doc_.world, view_), doc_.world.level.serialExecution, engine);
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
         &FrameTimings::snapshotMs, &FrameTimings::physicsApplyMs, &FrameTimings::physicsPrepareMs,
         &FrameTimings::physicsStepMs, &FrameTimings::renderSyncMs, &FrameTimings::renderMs, &FrameTimings::waitMs})
        averageTimings_.*member += weight * (measured.*member - averageTimings_.*member);
    return running;
}

void Editor::setShadowsEnabled(bool enabled) {
    shadowsEnabled_=enabled;
    sun_->setCastShadows(enabled);
    for(auto& render:renderObjects_) if(render.light)
        render.light->setCastShadows(enabled && render.light->getType()==Ogre::Light::LT_DIRECTIONAL);
}

void Editor::setSkyboxEnabled(bool enabled) {
    skyboxEnabled_ = enabled;
    sky_->setSky(&scene_, enabled);
}

void Editor::captureViewport(const std::filesystem::path& path) {
    Ogre::Image2 image;
    image.convertFromTexture(texture_,0,0);
    std::ofstream stream(path,std::ios::binary);
    stream << "P6\n" << image.getWidth() << ' ' << image.getHeight() << "\n255\n";
    for(unsigned y=0;y<image.getHeight();++y) for(unsigned x=0;x<image.getWidth();++x) {
        const auto colour=image.getColourAt(x,y,0);
        const char rgb[]={static_cast<char>(std::clamp(colour.r,0.0f,1.0f)*255),
                          static_cast<char>(std::clamp(colour.g,0.0f,1.0f)*255),
                          static_cast<char>(std::clamp(colour.b,0.0f,1.0f)*255)};
        stream.write(rgb,3);
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
    if (shape != viewShape_) {
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
        if (!own->prefab.empty()) target.overrides = &own->overrides;
    } else {
        target.overrides = &own->overrides;
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
            target.node = &it->node;
            target.overrides = it->node.prefab.empty() ? nullptr : &it->node.overrides;
        } else {
            target.overrides = &it->node.overrides;
        }
    }
    return target;
}

Editor::RunnerTarget Editor::runnerTarget() {
    RunnerTarget target;
    if (selected_.kind != Selection::Kind::Runner) return target;
    auto* file = fileOf(selected_.instance);
    if (selected_.instance < 0) {
        target.runner = doc_.world.level.findRunner(selected_.runner);
    } else if (!file) {
        return target;
    } else if (file->variant()) {
        target.overrides = &file->overrides;
        target.instance = selected_.path;
    } else if (selected_.path.empty()) {
        target.runner = file->findRunner(selected_.runner);
    } else if (auto* own = file->findNode(selected_.path[0])) {
        target.overrides = &own->overrides;
        target.instance.assign(selected_.path.begin() + 1, selected_.path.end());
    }
    // A runner an instance added is edited in place, inside that instance's overrides
    // (or those of a node it added, for that node's runners).
    while (target.overrides) {
        if (target.instance.empty()) {
            auto& added = target.overrides->addedRunners;
            const auto it = std::find_if(added.begin(), added.end(), [&](const Runner& each) { return each.id == selected_.runner; });
            if (it != added.end()) {
                target.runner = &*it;
                target.addedTo = target.overrides;
                target.overrides = nullptr;
            }
            break;
        }
        auto& nodes = target.overrides->added;
        const auto it = std::find_if(nodes.begin(), nodes.end(),
                                     [&](const AddedNode& each) { return each.node.id == target.instance[0]; });
        if (it == nodes.end()) break;
        target.overrides = &it->node.overrides;
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

const Runner* Editor::selectedRunner() {
    if (selected_.kind != Selection::Kind::Runner) return nullptr;
    if (selected_.instance < 0) return doc_.world.level.findRunner(selected_.runner);
    if (selected_.instance >= static_cast<int>(view_.instances.size())) return nullptr;
    for (const auto& expanded : view_.instances[selected_.instance].expansion.runners)
        if (expanded.instance == selected_.path && expanded.runner.id == selected_.runner) return &expanded.runner;
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
        scene_.destroyManualObject(it->mesh);
        scene_.destroySceneNode(it->node);
        pbs_.destroyDatablock(it->material->getName());
    }
    renderObjects_.clear();
    for (auto* root : sceneRoots_) scene_.destroySceneNode(root);
    sceneRoots_.clear();
    sceneNodes_.clear();
}

Ogre::TextureGpu* Editor::projectTexture(const std::string& path,int kind,bool mask) {
    if(path.empty() || !project_) return nullptr;
    const auto file=path.rfind("@ogre/",0)==0 ? std::filesystem::path(THRONE_OGRE_MEDIA_DIR)/path.substr(6) : project_->resolve(path);
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

void Editor::syncLighting() {
    const auto& r=doc_.world.level.rendering;
    if(!clustered_ || *clustered_!=r.clustered || clusterMin_!=r.minDistance || clusterMax_!=r.maxDistance) {
        if(r.clustered) scene_.setForwardClustered(true,16,8,24,96,0,0,r.minDistance,r.maxDistance);
        else scene_.setForward3D(true,4,4,5,96,r.minDistance,r.maxDistance);
        clustered_=r.clustered; clusterMin_=r.minDistance; clusterMax_=r.maxDistance;
    }
    scene_.getForwardPlus()->setDebugMode(r.debugClusters);
    sun_->setVisible(r.defaultSun);
    scene_.setAmbientLight(Ogre::ColourValue(r.ambientUpper.x,r.ambientUpper.y,r.ambientUpper.z),
        Ogre::ColourValue(r.ambientLower.x,r.ambientLower.y,r.ambientLower.z),toOgre(r.ambientDirection).normalisedCopy());
    // Reserve enough Forward slots for every authored rectangular light, including hidden ones.
    unsigned approx=0,ltc=0;
    for(const auto& instance:view_.instances) for(const auto& n:instance.expansion.nodes) if(n.node.light) {
        const auto type=n.node.light->type;
        if(type==LightType::AreaApprox || type==LightType::AreaLtc) {
            approx+=areaLightModel_==0 || (areaLightModel_<0 && type==LightType::AreaApprox);
            ltc+=areaLightModel_==1 || (areaLightModel_<0 && type==LightType::AreaLtc);
        }
    }
    pbs_.setAreaLightForwardSettings(static_cast<Ogre::uint16>(approx),static_cast<Ogre::uint16>(ltc));
}

void Editor::syncTransforms(const LevelExpansion* snapshot) {
    syncLighting();
    const auto& shown = snapshot ? *snapshot : view_;
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
        texture(Ogre::PBSM_REFLECTION,surface.reflectionMap,previous.reflectionMap,Ogre::CommonTextureTypes::EnvMap);
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
            light->setDiffuseColour(c.colour[0],c.colour[1],c.colour[2]);
            light->setSpecularColour(c.colour[0],c.colour[1],c.colour[2]);
            // Light::setDirection rotates its parent; reset the dedicated child to avoid roll drift.
            render.lightNode->setOrientation(Ogre::Quaternion::IDENTITY);
            light->setDirection(toOgre(c.direction).normalisedCopy());
            light->setCastShadows(shadowsEnabled_ && c.type==LightType::Directional);
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
    gridNode_->setVisible(showGrid_);
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
    const auto* node = edited ? edited->findNode(id) : nullptr;
    if (!node) return;
    if (node->prefab == file) applyOverrides(instance, id);
    else replacing_ = Replace{instance, id, file};
}

namespace {
/// Copies the properties @p from sets into @p into.
void assign(NodeOverride& into, const NodeOverride& from) {
    if (from.particles) into.particles = from.particles;
    if (from.material) into.material = from.material;
    if (from.light) into.light = from.light;
    if (from.sphere) into.sphere = from.sphere;
    if (from.capsule) into.capsule = from.capsule;
    if (from.plane) into.plane = from.plane;
    if (from.boxCollider) into.boxCollider = from.boxCollider;
    if (from.sphereCollider) into.sphereCollider = from.sphereCollider;
    if (from.capsuleCollider) into.capsuleCollider = from.capsuleCollider;
    if (from.planeCollider) into.planeCollider = from.planeCollider;
    if (from.rigidBody) into.rigidBody = from.rigidBody;

    if (from.name) into.name = from.name;
    if (from.position) into.position = from.position;
    if (from.rotation) into.rotation = from.rotation;
    if (from.scale) into.scale = from.scale;
    if (from.colour) into.colour = from.colour;
    if (from.behaviours) into.behaviours = from.behaviours;
    for (const auto& [name, fields] : from.scriptFields)
        for (const auto& [field, value] : fields) into.scriptFields[name][field] = value;
}

/// Writes the properties @p from sets into @p into's components, where it has them.
void assign(SceneNode& into, const NodeOverride& from) {
    applyExtraComponents(into, from);
    if (from.name) into.name = *from.name;
    if (from.behaviours) into.behaviours = *from.behaviours;
    for (const auto& [name, fields] : from.scriptFields)
        if (const auto component = into.scriptComponents.find(name); component != into.scriptComponents.end())
            for (const auto& [field, value] : fields) component->second.fields[field] = value;
    if (auto& transform = into.transform) {
        if (from.position) transform->position = *from.position;
        if (from.rotation) transform->rotation = *from.rotation;
        if (from.scale) transform->scale = *from.scale;
    }
    if (into.box && from.colour) into.box->colour = *from.colour;
}

/// Adds @p from on top of the overrides @p into already has.
void merge(Overrides& into, const Overrides& from) {
    for (const auto& entry : from.nodes) assign(into.node(entry.path), entry);
    for (const auto& entry : from.runners)
        for (const auto& [name, value] : entry.fields) into.fieldsOf(entry.instance, entry.runner, entry.behaviour).fields[name] = value;
    into.added.insert(into.added.end(), from.added.begin(), from.added.end());
    into.removed.insert(into.removed.end(), from.removed.begin(), from.removed.end());
    into.addedRunners.insert(into.addedRunners.end(), from.addedRunners.begin(), from.addedRunners.end());
    into.removedRunners.insert(into.removedRunners.end(), from.removedRunners.begin(), from.removedRunners.end());
}
}

void Editor::applyOverrides(int instance, NodeId id) {
    auto* edited = fileOf(instance);
    auto* node = edited ? edited->findNode(id) : nullptr;
    if (!node || node->prefab.empty() || node->overrides.empty()) return;
    Prefab prefab;
    try {
        prefab = doc_.prefabs.count(node->prefab) ? doc_.prefabs[node->prefab] : library_.prefab(node->prefab);
    } catch (const std::exception& error) {
        log(error.what());
        return;
    }
    // A variant keeps them as its own overrides; a prefab writes them into its nodes,
    // or into the overrides of its own instances for what lies deeper.
    if (prefab.variant()) {
        merge(prefab.overrides, node->overrides);
    } else {
        Overrides deeper;
        for (const auto& entry : node->overrides.nodes) {
            auto* target = entry.path.empty() ? &prefab.nodes.front() : prefab.findNode(entry.path[0]);
            if (!target) continue;
            if (entry.path.size() <= 1) {
                assign(*target, entry);
            } else {
                auto inner = entry;
                inner.path.erase(inner.path.begin());
                assign(target->overrides.node(inner.path), inner);
            }
        }
        // Added nodes join the prefab, or the overrides of its instance they were added inside.
        for (auto entry : node->overrides.added) {
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
        // Added runners join the prefab's; removed ones leave it, or the instance they come from.
        for (const auto& runner : node->overrides.addedRunners) {
            auto& made = prefab.addRunner(runner.name);
            const auto madeId = made.id;
            made = runner;
            made.id = madeId;
        }
        for (const auto& entry : node->overrides.removedRunners) {
            if (entry.instance.empty()) {
                prefab.removeRunner(entry.runner);
            } else if (auto* target = prefab.findNode(entry.instance[0])) {
                target->overrides.removedRunners.push_back({NodePath(entry.instance.begin() + 1, entry.instance.end()), entry.runner});
            }
        }
        // Removed nodes leave the prefab, or join the removals of its instance they lie in.
        for (const auto& path : node->overrides.removed) {
            if (path.size() == 1) {
                prefab.removeNode(path[0]);
            } else if (auto* target = prefab.findNode(path[0])) {
                target->overrides.removed.emplace_back(path.begin() + 1, path.end());
            }
        }
        for (const auto& entry : node->overrides.runners) {
            if (entry.instance.empty()) {
                if (auto* runner = prefab.findRunner(entry.runner); runner && entry.behaviour < runner->behaviours.size())
                    for (const auto& [name, value] : entry.fields) runner->behaviours[entry.behaviour].fields[name] = value;
            } else if (auto* target = prefab.findNode(entry.instance[0])) {
                const NodePath rest(entry.instance.begin() + 1, entry.instance.end());
                for (const auto& [name, value] : entry.fields)
                    target->overrides.fieldsOf(rest, entry.runner, entry.behaviour).fields[name] = value;
            }
        }
    }
    try {
        prefab.save(project_->resolve(node->prefab));
    } catch (const std::exception& error) {
        log("Could not write " + node->prefab + ": " + error.what());
        return;
    }
    auto before = doc_;
    if (doc_.prefabs.count(node->prefab)) doc_.prefabs[node->prefab] = prefab;
    library_.forget(node->prefab);
    node->overrides = {};
    commit(std::move(before));
    log("Applied the overrides to " + node->prefab);
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
        for (auto& attachment : plain[k].behaviours) refs(attachment.fields, 0, base);
        for (auto& [name, component] : plain[k].scriptComponents) refs(component.fields, 0, base);
    }
    for (auto& each : edited->nodes) {
        for (auto& attachment : each.behaviours) refs(attachment.fields, 0, {});
        for (auto& [name, component] : each.scriptComponents) refs(component.fields, 0, {});
    }
    // The instance's runners become the file's, by the instance they came from and their ID.
    std::map<std::pair<std::size_t, std::uint32_t>, std::uint32_t> runnerIds;
    for (const auto& expanded : expansion.runners) {
        const auto from = expansion.find(expanded.instance);
        if (expanded.instance.empty() || !ids.count(from)) continue;
        auto& made = edited->addRunner(expanded.runner.name);
        const auto madeId = made.id;
        made = expanded.runner;
        made.id = madeId;
        for (auto& behaviour : made.behaviours) refs(behaviour.fields, 0, expanded.instance);
        runnerIds[{from, expanded.runner.id}] = madeId;
    }
    for (auto& runner : edited->runners)
        for (auto& behaviour : runner.behaviours) refs(behaviour.fields, 0, {});
    // Level runners refer to nodes of the level's instances of this scene.
    for (const auto& placed : doc_.world.level.scenes) {
        const auto scene = doc_.world.scenes.find(placed.scene);
        if (scene == doc_.world.scenes.end() || &scene->second != edited) continue;
        for (auto& runner : doc_.world.level.runners)
            for (auto& behaviour : runner.behaviours) refs(behaviour.fields, placed.id, {});
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

void Editor::addRunner() {
    // Into an instance (the selected runner's, or the one holding the selected
    // node), the runner is kept in that instance's overrides.
    Overrides* into = nullptr;
    NodePath prefix;
    if (selected_.kind == Selection::Kind::Runner && selected_.instance >= 0) {
        const auto target = runnerTarget();
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
        Runner runner{newRunnerId(), "Runner", {}};
        into->addedRunners.push_back(runner);
        selected_ = {Selection::Kind::Runner, std::max(selected_.instance, 0), prefix, runner.id};
        commit(std::move(before));
        return;
    }
    try {
        auto before = doc_;
        // At the level, to the scene of the selection when it lies in one; else to the level.
        auto* file = selected_.kind != Selection::Kind::None ? fileOf(selected_.instance) : content();
        auto& owner = file ? static_cast<RunnerSet&>(*file) : doc_.world.level;
        auto& added = owner.addRunner("Runner");
        added.name = "Runner " + std::to_string(added.id);
        selected_ = {Selection::Kind::Runner, file ? std::max(selected_.instance, 0) : -1, {}, added.id};
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
            for (auto& [name, value] : attachment.fields)
                if (auto* ref = std::get_if<NodeRef>(&value); ref && !ref->scene && ref->node == original)
                    ref->node = added.id;
        };
        for (auto& attachment : added.behaviours) remap(attachment);
        for (auto& [name, component] : added.scriptComponents) remap(component);
        // Runners that refer to the node come along, referring to the copy instead
        // (the file's own, and at the level the level's naming this scene instance).
        const auto copyRunners = [&](RunnerSet& owner, std::uint32_t scene) {
            for (std::size_t i = 0, count = owner.runners.size(); i < count; ++i) {
                auto runner = owner.runners[i];
                bool refers = false;
                for (auto& behaviour : runner.behaviours)
                    for (auto& [name, value] : behaviour.fields)
                        if (auto* ref = std::get_if<NodeRef>(&value); ref && ref->scene == scene && ref->node == original) {
                            ref->node = added.id;
                            refers = true;
                        }
                if (!refers) continue;
                auto& made = owner.addRunner(runner.name);
                runner.id = made.id;
                if (runner.name.size() <= 250) runner.name += " Copy";
                made = std::move(runner);
            }
        };
        const auto copied = added.id;
        copyRunners(*file, 0);
        if (!content())
            if (const auto* placed = doc_.world.level.findScene(view_.instances[selected_.instance].id))
                copyRunners(doc_.world.level, placed->id);
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
    } else if (selected_.kind == Selection::Kind::Runner) {
        // One an instance added leaves its overrides; one it brought is removed by them.
        const auto target = runnerTarget();
        if (!target.runner || target.addedTo) {
            if (!target.addedTo && !target.overrides) return;
            checkpoint();
            if (target.addedTo) {
                auto& added = target.addedTo->addedRunners;
                added.erase(std::remove_if(added.begin(), added.end(), [&](const Runner& each) { return each.id == selected_.runner; }),
                            added.end());
            } else {
                target.overrides->removedRunners.push_back({target.instance, selected_.runner});
            }
            selected_ = {};
            return;
        }
        checkpoint();
        (selected_.instance < 0 ? static_cast<RunnerSet&>(doc_.world.level) : *fileOf(selected_.instance)).removeRunner(selected_.runner);
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
        loaded.world.level = Level::load(project_->resolve(levelPath_));
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
        dirty_ = false;
        selected_ = {};
        areaLightModel_=-1;
        refreshView();
        if (!view_.instances.empty() && !view_.instances[0].expansion.nodes.empty())
            selected_ = {Selection::Kind::Node, 0, view_.instances[0].expansion.nodes[0].path};
        focusSelection();
        const auto& r=doc_.world.level.rendering;
        setSkyboxEnabled(r.sky);
        showGrid_=r.grid;
        if(r.openingCamera) { selected_={}; target_=toOgre(r.target); yaw_=r.yaw; pitch_=r.pitch; distance_=r.distance; updateCamera(); }
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
        doc_.world.level.save(project_->resolve(levelPath_));
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
            doc_ = Document();
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
        if (ImGui::MenuItem("Add Runner")) addRunner();
        if (ImGui::MenuItem("Duplicate Node", "Ctrl+D", false, targetOf(selected_.instance, selected_.path).node))
            duplicate();
        if (ImGui::MenuItem("Delete", "Del", false, selectedNode() || selectedRunner())) erase();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Grid", nullptr, &showGrid_);
        ImGui::MenuItem("Statistics", nullptr, &showStatistics_);
        bool skybox = skyboxEnabled_;
        if (ImGui::MenuItem("Skybox", nullptr, &skybox)) setSkyboxEnabled(skybox);
        bool shadows=shadowsEnabled_;
        if(ImGui::MenuItem("Shadows",nullptr,&shadows)) setShadowsEnabled(shadows);
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
        bool defaultSun=r.defaultSun;
        if(ImGui::MenuItem("Default Sun",nullptr,&defaultSun)) { checkpoint(); r.defaultSun=defaultSun; }
        auto upper=r.ambientUpper,lower=r.ambientLower;
        if(ImGui::ColorEdit3("Ambient Upper",upper.data())) { checkpoint(); r.ambientUpper=upper; }
        if(ImGui::ColorEdit3("Ambient Lower",lower.data())) { checkpoint(); r.ambientLower=lower; }
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
                ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x - 290, origin.y + 8));
                ImGui::SetNextWindowBgAlpha(0.75f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
                if (ImGui::BeginChild("ViewportStatistics", ImVec2(280, statisticsCollapsed_ ? 28 : 200), ImGuiChildFlags_Borders,
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar)) {
                    if (ImGui::SmallButton(statisticsCollapsed_ ? "+" : "-")) statisticsCollapsed_ = !statisticsCollapsed_;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(statisticsCollapsed_ ? "Expand statistics" : "Minimize statistics");
                    ImGui::SameLine();
                    const auto fps = ImGui::GetIO().Framerate;
                    ImGui::Text("FPS %.1f | %.2f ms", fps, fps > 0 ? 1000.0f / fps : 0.0f);
                    if (!statisticsCollapsed_) {
                        const auto& t = averageTimings_;
                        ImGui::Text("Nodes %zu | Bodies %zu", renderObjects_.size(), physics_ ? physics_->bodyCount() : 0);
                        ImGui::Text("Particles ~%zu | Systems %zu",particleCount_,particleSystemCount_);
                        ImGui::Text("UI / scripts: %.2f / %.2f ms", t.uiMs, t.scriptMs);
                        ImGui::Text("Physics step: %.2f ms", t.physicsStepMs);
                        ImGui::Text("Physics apply/prepare: %.2f ms", t.physicsApplyMs + t.physicsPrepareMs);
                        ImGui::Text("Scene snapshot: %.2f ms", t.snapshotMs);
                        ImGui::Text("Render sync: %.2f ms", t.renderSyncMs);
                        ImGui::Text("Render / present: %.2f ms", t.renderMs);
                        ImGui::Text("Physics wait: %.2f ms", t.waitMs);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("CPU wall times; physics and rendering overlap.\nRender / present includes GPU and display waits.");
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
    camera_.setAspectRatio(static_cast<Ogre::Real>(viewportWidth_) / viewportHeight_);
    camera_.setProjectionType(orthographic_ ? Ogre::PT_ORTHOGRAPHIC : Ogre::PT_PERSPECTIVE);
    if (orthographic_)
        camera_.setOrthoWindowHeight(2 * distance_ * std::tan(camera_.getFOVy().valueRadians() / 2));
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
        if (render.shape.kind==PrimitiveKind::None) continue;
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
}

void Editor::createWorkspaces() {
    auto* compositor = root_.getCompositorManager2();
    viewportWorkspace_ = compositor->addWorkspace(&scene_, texture_, &camera_, viewportWorkspaceName, true);
    const Ogre::CompositorChannelVec channels{window_.getTexture(), texture_};
    windowWorkspace_ = compositor->addWorkspace(&scene_, channels, &camera_, windowWorkspaceName, true);
}

void Editor::destroyWorkspaces() {
    auto* compositor = root_.getCompositorManager2();
    for (auto** workspace : {&windowWorkspace_, &viewportWorkspace_}) {
        if (*workspace) compositor->removeWorkspace(*workspace);
        *workspace = nullptr;
    }
}

void Editor::log(const std::string& message) {
    messages_.push_back(message);
    if (messages_.size() > logLimit) messages_.erase(messages_.begin());
    Ogre::LogManager::getSingleton().logMessage("Editor: " + message);
}
}
