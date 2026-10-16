#include "editor/SceneEditor.h"
#include "editor/OgreMath.h"
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
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace throne {
namespace {
constexpr const char* viewportWorkspaceName = "ThroneEditor/Viewport";
constexpr const char* windowWorkspaceName = "ThroneEditor/Window";
constexpr const char* lineMaterial = "ThroneEditor/Lines";
constexpr float leftWidth = 220, rightWidth = 280, bottomHeight = 190;
constexpr std::size_t historyLimit = 100, logLimit = 200;

/// Returns the registered HlmsUnlit.
/// @throws std::runtime_error if it has not been registered.
Ogre::HlmsUnlit& unlitHlms(Ogre::Root& root) {
    auto* hlms = root.getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
    if (!hlms) throw std::runtime_error("SceneEditor requires HlmsUnlit");
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
Ogre::Matrix4 transform(const SceneObject& object) {
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

/// Begins a fixed panel; the caller must always call ImGui::End().
bool panel(const char* name, ImVec2 position, ImVec2 size, ImGuiWindowFlags flags = 0) {
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    return ImGui::Begin(name, nullptr, flags | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
}

/// Screen-space rectangle of the panels between the menu bar and the browser.
struct Layout {
    float top, width, height;
};

/// Computes the panel layout for the current display size.
Layout layout() {
    const auto& io = ImGui::GetIO();
    const float top = ImGui::GetFrameHeight();
    return {top, io.DisplaySize.x, std::max(io.DisplaySize.y - top - bottomHeight, 1.0f)};
}
}

SceneEditor::SceneEditor(Ogre::Root& root, Ogre::Window& window, Ogre::SceneManager& scene,
                         Ogre::Camera& camera)
    : root_(root), window_(window), scene_(scene), camera_(camera), unlit_(unlitHlms(root)),
      platform_(window), gui_(std::make_unique<Ogre::ImguiManager>()),
      guiPass_(std::make_unique<Ogre::CompositorPassImguiProvider>(gui_.get())) {
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
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
    log("Editor ready");
}

SceneEditor::~SceneEditor() {
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

void SceneEditor::frame(float deltaSeconds) {
    platform_.newFrame();
    ImGui::GetIO().DeltaTime = std::max(deltaSeconds, 1.0e-4f);
    ImGui::NewFrame();
    if (platform_.takeCloseRequest()) requestClose();
    shortcuts();
    menu();
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

void SceneEditor::rebuildScene() {
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

void SceneEditor::destroyRenderObjects() {
    for (const auto& render : renderObjects_) {
        scene_.destroyManualObject(render.mesh);
        scene_.destroySceneNode(render.node);
        unlit_.destroyDatablock(render.material->getName());
    }
    renderObjects_.clear();
}

void SceneEditor::syncTransforms() {
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

void SceneEditor::checkpoint() { commit(document_); }

void SceneEditor::commit(SceneDocument before) {
    undo_.push_back(std::move(before));
    if (undo_.size() > historyLimit) undo_.erase(undo_.begin());
    redo_.clear();
    dirty_ = true;
}

void SceneEditor::undo() {
    if (undo_.empty()) return;
    redo_.push_back(std::move(document_));
    document_ = std::move(undo_.back());
    undo_.pop_back();
    dirty_ = true;
    rebuildScene();
}

void SceneEditor::redo() {
    if (redo_.empty()) return;
    undo_.push_back(std::move(document_));
    document_ = std::move(redo_.back());
    redo_.pop_back();
    dirty_ = true;
    rebuildScene();
}

void SceneEditor::addCube() {
    try {
        auto before = document_;
        selected_ = document_.addCube().id;
        commit(std::move(before));
        rebuildScene();
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void SceneEditor::duplicate() {
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

void SceneEditor::erase() {
    auto& objects = document_.objects;
    const auto it = std::find_if(objects.begin(), objects.end(),
                                 [this](const auto& object) { return object.id == selected_; });
    if (it == objects.end()) return;
    checkpoint();
    document_.objects.erase(it);
    selected_ = 0;
    rebuildScene();
}

bool SceneEditor::open() {
    try {
        auto loaded = SceneDocument::load(std::filesystem::u8path(scenePath_));
        checkpoint();
        document_ = std::move(loaded);
        dirty_ = false;
        selected_ = document_.objects.empty() ? 0 : document_.objects.front().id;
        rebuildScene();
        focusSelection();
        log(std::string("Opened ") + scenePath_);
        return true;
    } catch (const std::exception& error) {
        fileError_ = error.what();
        log(std::string("Open failed: ") + fileError_);
        return false;
    }
}

bool SceneEditor::save() {
    try {
        document_.save(std::filesystem::u8path(scenePath_));
        dirty_ = false;
        log(std::string("Saved ") + scenePath_);
        return true;
    } catch (const std::exception& error) {
        fileError_ = error.what();
        log(std::string("Save failed: ") + fileError_);
        return false;
    }
}

void SceneEditor::shortcuts() {
    if (ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
        redo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) save();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) showFiles_ = true, saving_ = false;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) duplicate();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) erase();
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) focusSelection();
}

void SceneEditor::menu() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New")) {
            checkpoint();
            document_ = SceneDocument();
            selected_ = 0;
            rebuildScene();
        }
        if (ImGui::MenuItem("Open...", "Ctrl+O")) showFiles_ = true, saving_ = false;
        if (ImGui::MenuItem("Save", "Ctrl+S")) save();
        if (ImGui::MenuItem("Save As...")) showFiles_ = true, saving_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) requestClose();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
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
    ImGui::TextDisabled("%s%s", scenePath_, dirty_ ? " *" : "");
    ImGui::EndMainMenuBar();
}

void SceneEditor::fileDialog() {
    if (showFiles_) {
        ImGui::OpenPopup("Scene File");
        showFiles_ = false;
        fileError_.clear();
    }
    if (!ImGui::BeginPopupModal("Scene File", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::SetNextItemWidth(420);
    const bool enter = ImGui::InputText("Path", scenePath_, sizeof scenePath_,
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    if (!fileError_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", fileError_.c_str());
    if ((ImGui::Button(saving_ ? "Save" : "Open") || enter) && (saving_ ? save() : open()))
        ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void SceneEditor::requestClose() {
    if (dirty_) confirmClose_ = true;
    else close_ = true;
}

void SceneEditor::closeDialog() {
    if (confirmClose_) {
        ImGui::OpenPopup("Unsaved Changes");
        confirmClose_ = false;
        fileError_.clear();
    }
    if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("Save changes to %s before closing?", scenePath_);
    if (!fileError_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", fileError_.c_str());
    if (ImGui::Button("Save") && save()) close_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Discard")) close_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void SceneEditor::hierarchy() {
    const auto area = layout();
    if (panel("Hierarchy", {0, area.top}, {leftWidth, area.height})) {
        if (ImGui::Button("Add Cube")) addCube();
        ImGui::Separator();
        for (const auto& object : document_.objects) {
            ImGui::PushID(static_cast<int>(object.id));
            if (ImGui::Selectable(object.name.c_str(), object.id == selected_)) selected_ = object.id;
            ImGui::PopID();
        }
    }
    ImGui::End();
}

void SceneEditor::inspector() {
    const auto area = layout();
    if (panel("Inspector", {area.width - rightWidth, area.top}, {rightWidth, area.height})) {
        if (auto* object = document_.find(selected_)) {
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
            ImGui::Separator();
            if (ImGui::Button("Duplicate")) duplicate();
            ImGui::SameLine();
            if (ImGui::Button("Delete")) erase();
        } else {
            ImGui::TextDisabled("Nothing selected");
        }
    }
    ImGui::End();
}

void SceneEditor::viewport() {
    const auto area = layout();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (panel("Viewport", {leftWidth, area.top}, {area.width - leftWidth - rightWidth, area.height},
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

void SceneEditor::browser() {
    const auto area = layout();
    if (panel("Browser", {0, area.top + area.height}, {area.width, bottomHeight})) {
        if (ImGui::BeginTabBar("BrowserTabs")) {
            if (ImGui::BeginTabItem("Scenes")) {
                constexpr std::string_view suffix = ".throne.lton";
                std::error_code error;
                for (const auto& entry : std::filesystem::directory_iterator("scenes", error)) {
                    const auto file = entry.path().filename().u8string();
                    if (file.size() <= suffix.size() ||
                        file.compare(file.size() - suffix.size(), suffix.size(), suffix) != 0)
                        continue;
                    const auto path = entry.path().generic_u8string();
                    if (ImGui::Selectable(file.c_str()) && path.size() < sizeof scenePath_) {
                        std::memcpy(scenePath_, path.c_str(), path.size() + 1);
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

void SceneEditor::updateCamera() {
    const Ogre::Vector3 offset(std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_),
                               std::cos(pitch_) * std::cos(yaw_));
    camera_.setPosition(target_ + offset * distance_);
    camera_.lookAt(target_);
    camera_.setAspectRatio(static_cast<Ogre::Real>(viewportWidth_) / viewportHeight_);
    camera_.setProjectionType(orthographic_ ? Ogre::PT_ORTHOGRAPHIC : Ogre::PT_PERSPECTIVE);
    if (orthographic_)
        camera_.setOrthoWindowHeight(2 * distance_ * std::tan(camera_.getFOVy().valueRadians() / 2));
}

void SceneEditor::focusSelection() {
    const auto* object = document_.find(selected_);
    if (!object) return;
    target_ = toOgre(object->position);
    distance_ = std::max({3.0f, object->scale.x * 3, object->scale.y * 3, object->scale.z * 3});
}

void SceneEditor::pick(float x, float y) {
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

void SceneEditor::resizeViewport(unsigned width, unsigned height) {
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

void SceneEditor::createWorkspaces() {
    auto* compositor = root_.getCompositorManager2();
    viewportWorkspace_ = compositor->addWorkspace(&scene_, texture_, &camera_, viewportWorkspaceName, true);
    const Ogre::CompositorChannelVec channels{window_.getTexture(), texture_};
    windowWorkspace_ = compositor->addWorkspace(&scene_, channels, &camera_, windowWorkspaceName, true);
}

void SceneEditor::destroyWorkspaces() {
    auto* compositor = root_.getCompositorManager2();
    for (auto** workspace : {&windowWorkspace_, &viewportWorkspace_}) {
        if (*workspace) compositor->removeWorkspace(*workspace);
        *workspace = nullptr;
    }
}

void SceneEditor::log(const std::string& message) {
    messages_.push_back(message);
    if (messages_.size() > logLimit) messages_.erase(messages_.begin());
    Ogre::LogManager::getSingleton().logMessage("Editor: " + message);
}
}
