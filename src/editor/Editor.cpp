#include "editor/Editor.h"
#include "editor/OgreMath.h"
#include "editor/Lton.h"
#include "script/ScriptRuntime.h"
#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorNodeDef.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorWorkspaceDef.h>
#include <Compositor/Pass/OgreCompositorPassDef.h>
#include <OgreCamera.h>
#include <OgreCompositorPassImguiProvider.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsUnlit.h>
#include <OgreHlmsUnlitDatablock.h>
#include <OgreImguiManager.h>
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

/// Adds a unit cube centred on the origin. Faces are shaded by axis so the
/// unlit cube still reads as a solid.
void addCubeGeometry(Ogre::ManualObject& mesh, const std::string& material) {
    static constexpr float shade[3] = {0.78f, 1.0f, 0.62f};
    mesh.begin(material, Ogre::OT_TRIANGLE_LIST);
    Ogre::uint32 vertex = 0;
    for (int axis = 0; axis < 3; ++axis) {
        for (float side : {-0.5f, 0.5f}) {
            const int u = (axis + 1) % 3, v = (axis + 2) % 3;
            const float s = shade[axis] * (side < 0 ? 0.8f : 1.0f);
            for (int corner = 0; corner < 4; ++corner) {
                Ogre::Vector3 p;
                p[axis] = side;
                p[u] = corner == 1 || corner == 2 ? 0.5f : -0.5f;
                p[v] = corner >= 2 ? 0.5f : -0.5f;
                mesh.position(p);
                mesh.colour(s, s, s);
            }
            mesh.quad(vertex, vertex + 1, vertex + 2, vertex + 3);
            vertex += 4;
        }
    }
    mesh.end();
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
/// Inspector on the right, Browser below and Viewport in the middle.
void dockSpace() {
    const auto* viewport = ImGui::GetMainViewport();
    const ImGuiID id = ImGui::DockSpaceOverViewport(0, viewport);
    if (const auto* node = ImGui::DockBuilderGetNode(id);
        node && node->IsSplitNode() && ImGui::FindWindowSettingsByID(ImHashStr("Runners")))
        return;
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
    ImGui::DockBuilderDockWindow("Viewport", centre);
    ImGui::DockBuilderFinish(id);
}
}

Editor::Editor(Ogre::Root& root, Ogre::Window& window, Ogre::SceneManager& scene, Ogre::Camera& camera,
               const std::filesystem::path& project, std::filesystem::path projectList)
    : root_(root), window_(window), scene_(scene), camera_(camera), unlit_(unlitHlms(root)),
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
    compositor->createBasicWorkspaceDef(viewportWorkspaceName, Ogre::ColourValue(0.11f, 0.12f, 0.14f));
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
    addLines(*grid_, gridLines(), Ogre::ColourValue(0.35f, 0.37f, 0.40f));
    gridNode_ = attach(scene, grid_);
    outline_ = scene.createManualObject();
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
    // Play always runs the level (TDDja/06 6), on its expansion only.
    contexts_.resize(1);
    navigate_.reset();
    selected_ = {};
    refreshView();
    try {
        // Play runs the open level; playing the whole game (with main.lh) comes later.
        runtime_ = std::make_unique<ScriptRuntime>(
            doc_.world, view_, project_->root, [this](const std::string& line) { log(line); }, PlayScope::Level);
        log("Play");
    } catch (const ScriptError& error) {
        log(std::string("Play refused:\n") + error.what());
    }
}

void Editor::stop() {
    runtime_.reset();
    log("Stop");
}

Editor::~Editor() {
    // ImGui would save to io.IniFilename when its context is destroyed, after iniFile_ is gone.
    auto& io = ImGui::GetIO();
    if (io.IniFilename) ImGui::SaveIniSettingsToDisk(io.IniFilename);
    io.IniFilename = nullptr;
    destroyWorkspaces();
    root_.getCompositorManager2()->setCompositorPassProvider(nullptr);
    // ImGui materials reference the viewport texture, so release them first.
    guiPass_.reset();
    gui_.reset();
    root_.getRenderSystem()->getTextureGpuManager()->destroyTexture(texture_);
    destroyRenderObjects();
    for (auto* mesh : {grid_, outline_}) scene_.destroyManualObject(mesh);
    for (auto* node : {gridNode_, outlineNode_}) scene_.destroySceneNode(node);
    unlit_.destroyDatablock(lineMaterial);
}

void Editor::frame(float deltaSeconds) {
    platform_.newFrame();
    ImGui::GetIO().DeltaTime = std::max(deltaSeconds, 1.0e-4f);
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    if (platform_.takeCloseRequest()) requestLeave(Leave::Exit);
    if (!project_) {
        launcher();
        gui_->prepareForRender(&scene_);
        return;
    }
    if (runtime_) {
        try {
            runtime_->update(deltaSeconds);
        } catch (const ScriptError& error) {
            log(error.what());
            stop();
        }
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
    fileDialog();
    closeDialog();
    syncTransforms();
    updateCamera();
    gui_->prepareForRender(&scene_);
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

Editor::NodeTarget Editor::targetOf(int instance, const NodePath& path) {
    NodeTarget target;
    if (path.empty() && !(content() && content()->variant())) return target;
    if (!content()) {
        // At the level every node belongs to a scene instance, so edits become its overrides.
        if (instance < 0 || instance >= static_cast<int>(view_.instances.size())) return target;
        if (auto* scene = doc_.world.level.findScene(view_.instances[instance].id)) {
            target.overrides = &scene->overrides;
            target.relative = path;
        }
        return target;
    }
    // A variant has no nodes of its own: every edit is one of its overrides.
    if (content()->variant()) {
        target.overrides = &content()->overrides;
        target.relative = path;
        return target;
    }
    auto* own = content()->findNode(path[0]);
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

Editor::RunnerTarget Editor::runnerTarget() {
    RunnerTarget target;
    if (selected_.kind != Selection::Kind::Runner) return target;
    if (selected_.instance < 0) {
        target.runner = doc_.world.level.findRunner(selected_.runner);
    } else if (!content()) {
        if (selected_.instance < static_cast<int>(view_.instances.size()))
            if (auto* scene = doc_.world.level.findScene(view_.instances[selected_.instance].id)) {
                target.overrides = &scene->overrides;
                target.instance = selected_.path;
            }
    } else if (content()->variant()) {
        target.overrides = &content()->overrides;
        target.instance = selected_.path;
    } else if (selected_.path.empty()) {
        target.runner = content()->findRunner(selected_.runner);
    } else if (auto* own = content()->findNode(selected_.path[0])) {
        target.overrides = &own->overrides;
        target.instance.assign(selected_.path.begin() + 1, selected_.path.end());
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
    if (target.node) {
        if (changed & 1) target.node->name = value.name;
        if (changed & 2) target.node->position = value.position;
        if (changed & 4) target.node->rotation = value.rotation;
        if (changed & 8) target.node->scale = value.scale;
        if (changed & 16) {
            if (target.overrides) target.overrides->node({}).colour = value.colour;
            else target.node->colour = value.colour;
        }
    } else if (target.overrides) {
        auto& entry = target.overrides->node(target.relative);
        if (changed & 1) entry.name = value.name;
        if (changed & 2) entry.position = value.position;
        if (changed & 4) entry.rotation = value.rotation;
        if (changed & 8) entry.scale = value.scale;
        if (changed & 16) entry.colour = value.colour;
    }
}

void Editor::rebuildScene() {
    destroyRenderObjects();
    for (std::size_t i = 0; i < view_.instances.size(); ++i) {
        const auto& nodes = view_.instances[i].expansion.nodes;
        // Parents come first, so each node's OGRE parent already exists.
        std::vector<Ogre::SceneNode*> created(nodes.size());
        for (std::size_t k = 0; k < nodes.size(); ++k) {
            auto* parent = nodes[k].parent == Expansion::npos ? scene_.getRootSceneNode() : created[nodes[k].parent];
            created[k] = parent->createChildSceneNode();
            const auto name = "ThroneEditor/Node/" + std::to_string(i) + "/" + std::to_string(k);
            auto* material = createMaterial(unlit_, name);
            auto* mesh = scene_.createManualObject();
            addCubeGeometry(*mesh, name);
            created[k]->attachObject(mesh);
            renderObjects_.push_back({i, k, created[k], mesh, material});
        }
    }
    syncTransforms();
}

void Editor::destroyRenderObjects() {
    // Children before parents, so no node is destroyed while it still has children.
    for (auto it = renderObjects_.rbegin(); it != renderObjects_.rend(); ++it) {
        scene_.destroyManualObject(it->mesh);
        scene_.destroySceneNode(it->node);
        unlit_.destroyDatablock(it->material->getName());
    }
    renderObjects_.clear();
}

void Editor::syncTransforms() {
    const RenderObject* outlined = nullptr;
    for (const auto& render : renderObjects_) {
        const auto& expanded = view_.instances[render.instance].expansion.nodes[render.index];
        const auto& node = expanded.node;
        render.node->setPosition(toOgre(node.position));
        render.node->setOrientation(orientation(node.rotation));
        render.node->setScale(toOgre(node.scale));
        render.material->setColour(Ogre::ColourValue(node.colour[0], node.colour[1], node.colour[2]));
        if (selected_.kind == Selection::Kind::Node && selected_.instance == static_cast<int>(render.instance) &&
            selected_.path == expanded.path)
            outlined = &render;
    }
    gridNode_->setVisible(showGrid_);
    outlineNode_->setVisible(outlined != nullptr);
    if (outlined) {
        outlineNode_->setPosition(outlined->node->_getDerivedPositionUpdated());
        outlineNode_->setOrientation(outlined->node->_getDerivedOrientationUpdated());
        outlineNode_->setScale(outlined->node->_getDerivedScaleUpdated() * 1.02f);
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

void Editor::addNode() {
    auto* edited = content();
    if (!edited) {
        log("Nodes live in scenes: double-click a scene to edit it");
        return;
    }
    if (edited->variant()) {
        log("A variant only overrides its base; open the base to add nodes");
        return;
    }
    try {
        auto before = doc_;
        auto& added = edited->addNode("Cube", parentForNew());
        added.name = "Cube " + std::to_string(added.id);
        selected_ = {Selection::Kind::Node, 0, {added.id}};
        commit(std::move(before));
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Editor::addInstance() {
    auto* edited = content();
    const std::string prefab = newPrefabPath_;
    if (!edited || prefab.empty()) return;
    if (edited->variant()) {
        log("A variant only overrides its base; open the base to add instances");
        return;
    }
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
    if (!check.errors.empty()) {
        log("Cannot place " + prefab + ": " + check.errors.front());
        if (!before.prefabs.count(prefab)) library_.forget(prefab);
        doc_ = std::move(before);
        return;
    }
    try {
        auto& added = edited->addNode(std::filesystem::u8path(prefab).stem().stem().u8string(), parentForNew());
        added.prefab = prefab;
        added.position = {};
        selected_ = {Selection::Kind::Node, 0, {added.id}};
        commit(std::move(before));
    } catch (const std::exception& error) {
        log(error.what());
    }
}

std::uint32_t Editor::parentForNew() {
    // A node of the edited file can take a child; an instance cannot (yet).
    const auto* chosen = selectedNode();
    if (chosen && chosen->path.size() == 1 && chosen->node.prefab.empty()) return chosen->path[0];
    // A prefab keeps its single root.
    if (contexts_.back().kind == Context::Kind::Prefab && !content()->nodes.empty()) return content()->nodes.front().id;
    return 0;
}

bool Editor::isPrefabRoot(std::uint32_t id) {
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

void Editor::prefabify(std::uint32_t id) {
    auto* edited = content();
    auto* node = edited ? edited->findNode(id) : nullptr;
    if (!node || edited->variant()) return;
    if (isPrefabRoot(id)) {
        log("The root of a prefab cannot become a prefab; make a variant of the prefab instead");
        return;
    }
    // prefabs/<name>.prefab.lton, numbered when taken; characters a path cannot hold become '_'.
    std::string stem = node->name;
    for (auto& character : stem)
        if (std::string_view("<>:\"/\\|?*").find(character) != std::string_view::npos || character < 32) character = '_';
    std::string file;
    for (int number = 1;; ++number) {
        file = "prefabs/" + stem + (number > 1 ? " " + std::to_string(number) : "") + ".prefab.lton";
        if (!doc_.prefabs.count(file) && !std::filesystem::exists(project_->resolve(file))) break;
    }
    // An instance becomes a variant of its prefab with its overrides; a plain node takes its subtree along.
    Prefab made;
    std::set<std::uint32_t> moved;
    if (!node->prefab.empty()) {
        made.base = node->prefab;
        made.overrides = node->overrides;
    } else {
        for (const auto& each : edited->nodes)
            if (edited->isWithin(each.id, id)) made.nodes.push_back(each);
        made.nodes.front().parent = 0;
        made.nextNodeId = edited->nextNodeId;
        for (const auto& each : made.nodes)
            if (each.id != id) moved.insert(each.id);
    }
    try {
        made.save(project_->resolve(file));
    } catch (const std::exception& error) {
        log("Could not write " + file + ": " + error.what());
        return;
    }
    library_.forget(file);
    auto before = doc_;
    // Paths into the moved nodes now pass through the new instance.
    const auto through = [&](NodePath& path) {
        if (!path.empty() && moved.count(path[0])) path.insert(path.begin(), id);
    };
    const auto refs = [&](std::map<std::string, FieldValue>& fields, std::uint32_t scene) {
        for (auto& [name, value] : fields)
            if (auto* ref = std::get_if<NodeRef>(&value); ref && ref->scene == scene && moved.count(ref->node)) {
                ref->inner.insert(ref->inner.begin(), ref->node);
                ref->node = id;
            }
    };
    for (auto& runner : edited->runners)
        for (auto& behaviour : runner.behaviours) refs(behaviour.fields, 0);
    // The level's instances of an edited scene hold overrides and references into it.
    if (contexts_.back().kind == Context::Kind::Scene)
        for (auto& instance : doc_.world.level.scenes) {
            if (instance.scene != contexts_.back().path) continue;
            for (auto& entry : instance.overrides.nodes) through(entry.path);
            for (auto& entry : instance.overrides.runners) {
                if (entry.instance.empty()) refs(entry.fields, 0);
                through(entry.instance);
            }
            for (auto& runner : doc_.world.level.runners)
                for (auto& behaviour : runner.behaviours) refs(behaviour.fields, instance.id);
        }
    for (const auto child : moved)
        if (auto* each = edited->findNode(child); each && each->parent == id) edited->removeNode(child);
    node = edited->findNode(id);
    node->prefab = file;
    node->overrides = {};
    selected_ = {Selection::Kind::Node, 0, {id}};
    commit(std::move(before));
    log("Created " + file);
}

void Editor::addRunner() {
    try {
        auto before = doc_;
        auto& owner = content() ? static_cast<RunnerSet&>(*content()) : doc_.world.level;
        auto& added = owner.addRunner("Runner");
        added.name = "Runner " + std::to_string(added.id);
        selected_ = {Selection::Kind::Runner, content() ? 0 : -1, {}, added.id};
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
    if (!target.node || isPrefabRoot(target.node->id)) return;
    try {
        auto before = doc_;
        auto copy = *target.node;
        auto& added = content()->addNode(copy.name, copy.parent);
        copy.id = added.id;
        if (copy.name.size() <= 250) copy.name += " Copy";
        copy.position.x += 1;
        added = std::move(copy);
        selected_.path = {added.id};
        commit(std::move(before));
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Editor::erase() {
    if (selected_.kind == Selection::Kind::Node) {
        if (!targetOf(selected_.instance, selected_.path).node) {
            log("This node comes from an instance: open its prefab or scene to delete it");
            return;
        }
        if (isPrefabRoot(selected_.path[0])) {
            log("A prefab keeps its root node");
            return;
        }
        checkpoint();
        content()->removeNode(selected_.path[0]);
    } else if (selected_.kind == Selection::Kind::Runner) {
        if (!runnerTarget().runner) {
            log("This runner comes from an instance: open its prefab or scene to delete it");
            return;
        }
        checkpoint();
        (selected_.instance < 0 ? static_cast<RunnerSet&>(doc_.world.level) : *content()).removeRunner(selected_.runner);
    } else {
        return;
    }
    selected_ = {};
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
        refreshView();
        if (!view_.instances.empty() && !view_.instances[0].expansion.nodes.empty())
            selected_ = {Selection::Kind::Node, 0, view_.instances[0].expansion.nodes[0].path};
        focusSelection();
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
    if (ImGui::GetIO().WantTextInput) return;
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
        if (ImGui::MenuItem("Add Node", nullptr, false, content() != nullptr)) addNode();
        if (ImGui::MenuItem("Add Runner")) addRunner();
        if (ImGui::MenuItem("Duplicate Node", "Ctrl+D", false, targetOf(selected_.instance, selected_.path).node))
            duplicate();
        if (ImGui::MenuItem("Delete", "Del", false, selectedNode() || selectedRunner())) erase();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Grid", nullptr, &showGrid_);
        ImGui::MenuItem("Orthographic", nullptr, &orthographic_);
        if (ImGui::MenuItem("Focus Selection", "F")) focusSelection();
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem(runtime_ ? "Stop" : "Play", "F5")) runtime_ ? stop() : play();
    if (runtime_) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "Playing");
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
    if (runtime_) stop();
    leaving_ = action;
    if (dirty_) confirmClose_ = true;
    else leave(action);
}

void Editor::leave(Leave action) {
    if (action == Leave::Exit) close_ = true;
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin("Viewport", nullptr,
              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const ImVec2 size = ImGui::GetContentRegionAvail();
        if (size.x >= 1 && size.y >= 1) {
            resizeViewport(static_cast<unsigned>(size.x), static_cast<unsigned>(size.y));
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Image(texture_, size);
            // ImGuizmo will not start a drag while another item was hovered in the previous
            // frame, so the camera/picking input steps aside while the gizmo is under the mouse.
            if (!gizmoOver_ && !ImGuizmo::IsUsing()) {
                ImGui::SetCursorScreenPos(origin);
                ImGui::SetNextItemAllowOverlap();
                ImGui::InvisibleButton("ViewportInput", size, ImGuiButtonFlags_MouseButtonLeft |
                                                                  ImGuiButtonFlags_MouseButtonRight |
                                                                  ImGuiButtonFlags_MouseButtonMiddle);
                const auto& io = ImGui::GetIO();
                if (ImGui::IsItemHovered() && io.MouseWheel != 0)
                    distance_ = std::clamp(distance_ * std::pow(0.88f, io.MouseWheel), 0.5f, 500.0f);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    pick((io.MousePos.x - origin.x) / size.x, (io.MousePos.y - origin.y) / size.y);
                if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                    yaw_ -= io.MouseDelta.x * 0.01f;
                    pitch_ = std::clamp(pitch_ + io.MouseDelta.y * 0.01f, -1.5f, 1.5f);
                }
                if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Middle))
                    target_ += (camera_.getUp() * io.MouseDelta.y - camera_.getRight() * io.MouseDelta.x) *
                               (distance_ * 0.0015f);
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
        }
    }
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
                    if (variant) ImGui::PopStyleColor();
                }
                // A node dropped anywhere on the Prefabs tab becomes a prefab (TDDja/06 7).
                if (kind == Context::Kind::Prefab && !runtime_ &&
                    ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->InnerRect, ImGui::GetID("prefabify"))) {
                    if (const auto* payload = ImGui::AcceptDragDropPayload(nodePayload))
                        prefabify(*static_cast<const std::uint32_t*>(payload->Data));
                    ImGui::EndDragDropTarget();
                }
                ImGui::EndTabItem();
            };
            files("Scenes", "scenes", ".scene.lton", newScenePath_, sizeof newScenePath_, Context::Kind::Scene);
            files("Prefabs", "prefabs", ".prefab.lton", newPrefabPath_, sizeof newPrefabPath_, Context::Kind::Prefab);
            if (ImGui::BeginTabItem("Log")) {
                for (const auto& message : messages_) ImGui::TextUnformatted(message.c_str());
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void Editor::gizmo(ImVec2 origin, ImVec2 size) {
    const auto* node = selectedNode();
    const RenderObject* render = nullptr;
    for (const auto& each : renderObjects_)
        if (node && selected_.instance == static_cast<int>(each.instance) &&
            view_.instances[each.instance].expansion.nodes[each.index].path == selected_.path)
            render = &each;
    if (!render || runtime_) {
        gizmoOver_ = false;
        return;
    }
    float view[16], projection[16], model[16];
    toColumnMajor(camera_.getViewMatrix(), view);
    toColumnMajor(camera_.getProjectionMatrix(), projection);
    toColumnMajor(render->node->_getFullTransformUpdated(), model);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
    ImGuizmo::SetOrthographic(orthographic_);
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
    auto* parent = render->node->getParentSceneNode();
    const auto local = parent->_getFullTransformUpdated().inverseAffine() * fromColumnMajor(model);
    Ogre::Vector3 position, scale;
    Ogre::Quaternion rotation;
    local.decomposition(position, scale, rotation);
    Ogre::Matrix3 axes;
    rotation.ToRotationMatrix(axes);
    Ogre::Radian z, y, x;
    axes.ToEulerAnglesZYX(z, y, x);
    auto value = node->node;
    value.position = fromOgre(position);
    value.rotation = {x.valueDegrees(), y.valueDegrees(), z.valueDegrees()};
    value.scale = fromOgre(scale);
    for (float* component : {&value.scale.x, &value.scale.y, &value.scale.z}) *component = std::max(*component, 0.001f);
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
    for (const auto& render : renderObjects_) {
        if (selected_.kind != Selection::Kind::Node || selected_.instance != static_cast<int>(render.instance) ||
            view_.instances[render.instance].expansion.nodes[render.index].path != selected_.path)
            continue;
        target_ = render.node->_getDerivedPositionUpdated();
        const auto scale = render.node->_getDerivedScaleUpdated();
        distance_ = std::max({Ogre::Real(3), scale.x * 3, scale.y * 3, scale.z * 3});
    }
}

void Editor::pick(float x, float y) {
    const Ogre::Ray ray = camera_.getCameraToViewportRay(x, y);
    const Ogre::AxisAlignedBox unit(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);
    auto nearest = std::numeric_limits<Ogre::Real>::max();
    selected_ = {};
    for (const auto& render : renderObjects_) {
        // Test in node space; the unnormalized direction keeps distances comparable.
        const auto inverse = render.node->_getFullTransformUpdated().inverseAffine();
        const Ogre::Ray local(inverse.transformAffine(ray.getOrigin()),
                              inverse.transformDirectionAffine(ray.getDirection()));
        const auto hit = local.intersects(unit);
        if (hit.first && hit.second < nearest) {
            nearest = hit.second;
            selected_ = {Selection::Kind::Node, static_cast<int>(render.instance),
                         view_.instances[render.instance].expansion.nodes[render.index].path};
        }
    }
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
