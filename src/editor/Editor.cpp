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
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace throne {
namespace {
constexpr const char* viewportWorkspaceName = "ThroneEditor/Viewport";
constexpr const char* windowWorkspaceName = "ThroneEditor/Window";
constexpr const char* lineMaterial = "ThroneEditor/Lines";
constexpr std::size_t historyLimit = 100, logLimit = 200;

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

/// Returns the world transform of @p object.
Ogre::Matrix4 transform(const LevelObject& object) {
    Ogre::Matrix4 result;
    result.makeTransform(toOgre(object.position), toOgre(object.scale), orientation(object.rotation));
    return result;
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

/// Fills the window below the menu bar with a dock space. The first time (no
/// saved layout) it docks the panels as hierarchy left, inspector right, browser
/// below and viewport in the middle.
void dockSpace() {
    const auto* viewport = ImGui::GetMainViewport();
    const ImGuiID id = ImGui::DockSpaceOverViewport(0, viewport);
    if (const auto* node = ImGui::DockBuilderGetNode(id); node && node->IsSplitNode()) return;
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, viewport->WorkSize);
    ImGuiID centre = id;
    const ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.18f, nullptr, &centre);
    const ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.26f, nullptr, &centre);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.27f, nullptr, &centre);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
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
        document_ = std::filesystem::exists(project_->resolve(level)) ? Level::load(project_->resolve(level)) : Level();
        undo_.clear();
        redo_.clear();
        dirty_ = false;
        selected_ = document_.objects.empty() ? 0 : document_.objects.front().id;
        rebuildScene();
        focusSelection();
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
    document_ = Level();
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    selected_ = 0;
    rebuildScene();
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
    playBackup_ = document_;
    try {
        runtime_ = std::make_unique<ScriptRuntime>(document_, project_->root,
                                                   [this](const std::string& line) { log(line); });
        log("Play");
    } catch (const ScriptError& error) {
        document_ = std::move(playBackup_);
        log(std::string("Play refused:\n") + error.what());
    }
}

void Editor::stop() {
    runtime_.reset();
    document_ = std::move(playBackup_);
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
    shortcuts();
    menu();
    dockSpace();
    hierarchy();
    inspector();
    viewport();
    browser();
    fileDialog();
    closeDialog();
    syncTransforms();
    updateCamera();
    gui_->prepareForRender(&scene_);
}

void Editor::rebuildScene() {
    destroyRenderObjects();
    for (const auto& object : document_.objects) {
        const auto name = "ThroneEditor/Object/" + std::to_string(object.id);
        auto* material = createMaterial(unlit_, name);
        auto* mesh = scene_.createManualObject();
        addCubeGeometry(*mesh, name);
        renderObjects_.push_back({attach(scene_, mesh), mesh, material});
    }
    syncTransforms();
}

void Editor::destroyRenderObjects() {
    for (const auto& render : renderObjects_) {
        scene_.destroyManualObject(render.mesh);
        scene_.destroySceneNode(render.node);
        unlit_.destroyDatablock(render.material->getName());
    }
    renderObjects_.clear();
}

void Editor::syncTransforms() {
    for (std::size_t i = 0; i < renderObjects_.size(); ++i) {
        const auto& object = document_.objects[i];
        const auto& render = renderObjects_[i];
        render.node->setPosition(toOgre(object.position));
        render.node->setOrientation(orientation(object.rotation));
        render.node->setScale(toOgre(object.scale));
        render.material->setColour(Ogre::ColourValue(object.colour[0], object.colour[1], object.colour[2]));
    }
    gridNode_->setVisible(showGrid_);
    const auto* selected = document_.find(selected_);
    outlineNode_->setVisible(selected != nullptr);
    if (selected) {
        outlineNode_->setPosition(toOgre(selected->position));
        outlineNode_->setOrientation(orientation(selected->rotation));
        outlineNode_->setScale(toOgre(selected->scale) * 1.02f);
    }
}

void Editor::checkpoint() { commit(document_); }

void Editor::commit(Level before) {
    undo_.push_back(std::move(before));
    if (undo_.size() > historyLimit) undo_.erase(undo_.begin());
    redo_.clear();
    dirty_ = true;
}

void Editor::undo() {
    if (undo_.empty()) return;
    redo_.push_back(std::move(document_));
    document_ = std::move(undo_.back());
    undo_.pop_back();
    dirty_ = true;
    rebuildScene();
}

void Editor::redo() {
    if (redo_.empty()) return;
    undo_.push_back(std::move(document_));
    document_ = std::move(redo_.back());
    redo_.pop_back();
    dirty_ = true;
    rebuildScene();
}

void Editor::addCube() {
    try {
        auto before = document_;
        selected_ = document_.addCube().id;
        commit(std::move(before));
        rebuildScene();
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Editor::duplicate() {
    const auto* source = document_.find(selected_);
    if (!source) return;
    try {
        auto before = document_;
        auto copy = *source;
        auto& added = document_.addCube();
        copy.id = added.id;
        if (copy.name.size() <= 250) copy.name += " Copy";
        copy.position.x += 1;
        added = std::move(copy);
        selected_ = added.id;
        commit(std::move(before));
        rebuildScene();
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Editor::erase() {
    auto& objects = document_.objects;
    const auto it = std::find_if(objects.begin(), objects.end(),
                                 [this](const auto& object) { return object.id == selected_; });
    if (it == objects.end()) return;
    checkpoint();
    document_.objects.erase(it);
    selected_ = 0;
    rebuildScene();
}

bool Editor::open() {
    try {
        auto loaded = Level::load(project_->resolve(levelPath_));
        checkpoint();
        document_ = std::move(loaded);
        dirty_ = false;
        selected_ = document_.objects.empty() ? 0 : document_.objects.front().id;
        rebuildScene();
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
        document_.save(project_->resolve(levelPath_));
        dirty_ = false;
        log(std::string("Saved ") + levelPath_);
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
}

void Editor::menu() {
    if (!ImGui::BeginMainMenuBar()) return;
    const bool editing = !runtime_;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", nullptr, false, editing)) {
            checkpoint();
            document_ = Level();
            selected_ = 0;
            rebuildScene();
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
        const bool hasSelection = document_.find(selected_) != nullptr;
        if (ImGui::MenuItem("Add Cube")) addCube();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSelection)) duplicate();
        if (ImGui::MenuItem("Delete", "Del", false, hasSelection)) erase();
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

void Editor::hierarchy() {
    if (ImGui::Begin("Hierarchy")) {
        ImGui::BeginDisabled(runtime_ != nullptr);
        if (ImGui::Button("Add Cube")) addCube();
        ImGui::EndDisabled();
        ImGui::Separator();
        for (const auto& object : document_.objects) {
            ImGui::PushID(static_cast<int>(object.id));
            if (ImGui::Selectable(object.name.c_str(), object.id == selected_)) selected_ = object.id;
            ImGui::PopID();
        }
    }
    ImGui::End();
}

void Editor::inspector() {
    if (ImGui::Begin("Inspector")) {
        if (auto* object = document_.find(selected_)) {
            ImGui::BeginDisabled(runtime_ != nullptr);
            // One undo step per widget interaction, recorded when the widget is released.
            const auto track = [this] {
                if (ImGui::IsItemActivated()) editStart_ = document_;
                if (ImGui::IsItemDeactivatedAfterEdit()) commit(std::move(editStart_));
            };
            constexpr auto clamp = ImGuiSliderFlags_AlwaysClamp;
            char name[256];
            std::snprintf(name, sizeof name, "%s", object->name.c_str());
            if (ImGui::InputText("Name", name, sizeof name) && name[0]) object->name = name;
            track();
            ImGui::DragFloat3("Position", object->position.data(), 0.05f, -1.0e6f, 1.0e6f, "%.3f", clamp);
            track();
            ImGui::DragFloat3("Rotation", object->rotation.data(), 0.5f, -1.0e6f, 1.0e6f, "%.1f", clamp);
            track();
            ImGui::DragFloat3("Scale", object->scale.data(), 0.01f, 0.001f, 1.0e6f, "%.3f", clamp);
            track();
            ImGui::ColorEdit3("Colour", object->colour.data());
            track();
            char script[256];
            std::snprintf(script, sizeof script, "%s", object->script.c_str());
            if (ImGui::InputTextWithHint("Script", "scripts/....lh", script, sizeof script)) object->script = script;
            track();
            ImGui::Separator();
            if (ImGui::Button("Duplicate")) duplicate();
            ImGui::SameLine();
            if (ImGui::Button("Delete")) erase();
            ImGui::EndDisabled();
        } else {
            ImGui::TextDisabled("Nothing selected");
        }
    }
    ImGui::End();
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
            ImGui::SetCursorScreenPos(origin);
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
    const auto* object = document_.find(selected_);
    if (!object) return;
    target_ = toOgre(object->position);
    distance_ = std::max({3.0f, object->scale.x * 3, object->scale.y * 3, object->scale.z * 3});
}

void Editor::pick(float x, float y) {
    const Ogre::Ray ray = camera_.getCameraToViewportRay(x, y);
    const Ogre::AxisAlignedBox unit(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);
    auto nearest = std::numeric_limits<Ogre::Real>::max();
    selected_ = 0;
    for (const auto& object : document_.objects) {
        // Test in object space; the unnormalized direction keeps distances comparable.
        const auto inverse = transform(object).inverseAffine();
        const Ogre::Ray local(inverse.transformAffine(ray.getOrigin()),
                              inverse.transformDirectionAffine(ray.getDirection()));
        const auto hit = local.intersects(unit);
        if (hit.first && hit.second < nearest) {
            nearest = hit.second;
            selected_ = object.id;
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
