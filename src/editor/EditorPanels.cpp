// The Scene, Runners and Inspector panels of the Editor (TDDja/05 6).
#include "editor/Editor.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <optional>

namespace throne {
namespace {
/// Drag-and-drop payload naming a scene node.
struct NodePayload {
    int scene;
    std::uint32_t id;
};
constexpr const char* nodePayload = "THRONE_NODE";

/// A pending reparent, applied after the tree has been drawn.
struct Move {
    int scene;
    std::uint32_t id, parent;
};
std::optional<Move> pendingMove;

/// Accepts a node dropped on the last item, from the same scene, to go under @p parent.
void dropTarget(int scene, std::uint32_t parent) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const auto* payload = ImGui::AcceptDragDropPayload(nodePayload)) {
        const auto& node = *static_cast<const NodePayload*>(payload->Data);
        if (node.scene == scene) pendingMove = Move{scene, node.id, parent};
    }
    ImGui::EndDragDropTarget();
}

/// Copies @p text into a fixed buffer for an InputText.
template <std::size_t Size>
void fill(char (&buffer)[Size], const std::string& text) {
    std::snprintf(buffer, Size, "%s", text.c_str());
}
}

void Editor::scenePanel() {
    if (ImGui::Begin("Scene")) {
        ImGui::BeginDisabled(runtime_ != nullptr);
        if (ImGui::Button("Add Node")) addNode();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-90);
        ImGui::InputText("##ScenePath", newScenePath_, sizeof newScenePath_);
        ImGui::SameLine();
        if (ImGui::Button("Add Scene")) addScene();
        ImGui::EndDisabled();
        ImGui::Separator();
        if (world_.scenes.empty()) ImGui::TextDisabled("No scenes. Add one above.");
        for (std::size_t i = 0; i < world_.scenes.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const bool open = ImGui::TreeNodeEx("##scene", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                                "%s", world_.scenes[i].path.c_str());
            dropTarget(static_cast<int>(i), 0);
            if (open) {
                nodeTree(i, 0);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (pendingMove && !runtime_) {
            auto before = world_;
            if (world_.scenes[pendingMove->scene].scene.reparent(pendingMove->id, pendingMove->parent)) {
                commit(std::move(before));
                rebuildScene();
            }
        }
        pendingMove.reset();
    }
    ImGui::End();
}

void Editor::nodeTree(std::size_t scene, std::uint32_t parent) {
    const auto& prefab = world_.scenes[scene].scene;
    for (const auto& node : prefab.nodes) {
        if (node.parent != parent) continue;
        const bool leaf = std::none_of(prefab.nodes.begin(), prefab.nodes.end(),
                                       [&](const SceneNode& other) { return other.parent == node.id; });
        const bool chosen = selected_.kind == Selection::Kind::Node && selected_.scene == static_cast<int>(scene) &&
                            selected_.id == node.id;
        auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
        if (leaf) flags |= ImGuiTreeNodeFlags_Leaf;
        if (chosen) flags |= ImGuiTreeNodeFlags_Selected;
        const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(node.id)), flags, "%s",
                                            node.name.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            selected_ = {Selection::Kind::Node, static_cast<int>(scene), node.id};
        if (ImGui::BeginDragDropSource()) {
            const NodePayload payload{static_cast<int>(scene), node.id};
            ImGui::SetDragDropPayload(nodePayload, &payload, sizeof payload);
            ImGui::TextUnformatted(node.name.c_str());
            ImGui::EndDragDropSource();
        }
        dropTarget(static_cast<int>(scene), node.id);
        if (open) {
            nodeTree(scene, node.id);
            ImGui::TreePop();
        }
    }
}

void Editor::runnersPanel() {
    if (ImGui::Begin("Runners")) {
        const auto section = [&](int scene, const char* label) {
            ImGui::PushID(scene);
            const bool open = ImGui::TreeNodeEx("##runners", ImGuiTreeNodeFlags_DefaultOpen, "%s", label);
            ImGui::SameLine();
            ImGui::BeginDisabled(runtime_ != nullptr);
            if (ImGui::SmallButton("+ Runner")) addRunner(scene);
            ImGui::EndDisabled();
            if (open) {
                for (const auto& runner : runnersOf(scene).runners) {
                    ImGui::PushID(static_cast<int>(runner.id));
                    const bool chosen = selected_.kind == Selection::Kind::Runner && selected_.scene == scene &&
                                        selected_.id == runner.id;
                    if (ImGui::Selectable(runner.name.c_str(), chosen))
                        selected_ = {Selection::Kind::Runner, scene, runner.id};
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        };
        section(-1, "Level");
        for (std::size_t i = 0; i < world_.scenes.size(); ++i) section(static_cast<int>(i), world_.scenes[i].path.c_str());
    }
    ImGui::End();
}

void Editor::inspector() {
    if (ImGui::Begin("Inspector")) {
        ImGui::BeginDisabled(runtime_ != nullptr);
        if (auto* node = selectedNode()) nodeInspector(*node);
        else if (auto* runner = selectedRunner()) runnerInspector(*runner, selected_.scene);
        else ImGui::TextDisabled("Nothing selected");
        ImGui::EndDisabled();
    }
    ImGui::End();
}

void Editor::nodeInspector(SceneNode& node) {
    // One undo step per widget interaction, recorded when the widget is released.
    const auto track = [this] {
        if (ImGui::IsItemActivated()) editStart_ = world_;
        if (ImGui::IsItemDeactivatedAfterEdit()) commit(std::move(editStart_));
    };
    constexpr auto clamp = ImGuiSliderFlags_AlwaysClamp;
    ImGui::TextDisabled("Scene node");
    char name[256];
    fill(name, node.name);
    if (ImGui::InputText("Name", name, sizeof name) && name[0]) node.name = name;
    track();
    ImGui::DragFloat3("Position", node.position.data(), 0.05f, -1.0e6f, 1.0e6f, "%.3f", clamp);
    track();
    ImGui::DragFloat3("Rotation", node.rotation.data(), 0.5f, -1.0e6f, 1.0e6f, "%.1f", clamp);
    track();
    ImGui::DragFloat3("Scale", node.scale.data(), 0.01f, 0.001f, 1.0e6f, "%.3f", clamp);
    track();
    ImGui::ColorEdit3("Colour", node.colour.data());
    track();
    ImGui::Separator();
    if (ImGui::Button("Duplicate")) duplicate();
    ImGui::SameLine();
    if (ImGui::Button("Delete")) erase();
}

void Editor::runnerInspector(Runner& runner, int scene) {
    const auto track = [this] {
        if (ImGui::IsItemActivated()) editStart_ = world_;
        if (ImGui::IsItemDeactivatedAfterEdit()) commit(std::move(editStart_));
    };
    ImGui::TextDisabled(scene < 0 ? "Level runner" : "Scene runner");
    char text[256];
    fill(text, runner.name);
    if (ImGui::InputText("Name", text, sizeof text) && text[0]) runner.name = text;
    track();

    std::optional<std::size_t> remove;
    for (std::size_t k = 0; k < runner.behaviours.size(); ++k) {
        auto& behaviour = runner.behaviours[k];
        ImGui::PushID(static_cast<int>(k));
        const auto title = (behaviour.script.empty() ? std::string("(no script)") : behaviour.script) + "###behaviour";
        if (ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            fill(text, behaviour.script);
            if (ImGui::InputTextWithHint("Script", "scripts/....lh", text, sizeof text)) behaviour.script = text;
            track();
            const auto& cached = fieldsOf(behaviour.script);
            if (!cached.error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", cached.error.c_str());
            for (const auto& info : cached.fields) {
                ImGui::PushID(info.name.c_str());
                const auto found = behaviour.fields.find(info.name);
                const bool overridden = found != behaviour.fields.end();
                // A stored value of the wrong type (the script changed) shows the default.
                const auto& value =
                    overridden && found->second.index() == info.defaultValue.index() ? found->second : info.defaultValue;
                switch (info.kind) {
                case FieldKind::Number: {
                    auto number = static_cast<float>(std::get<double>(value));
                    if (ImGui::DragFloat(info.name.c_str(), &number, 0.1f)) behaviour.fields[info.name] = double(number);
                    track();
                    break;
                }
                case FieldKind::Bool: {
                    bool flag = std::get<bool>(value);
                    if (ImGui::Checkbox(info.name.c_str(), &flag)) {
                        checkpoint();
                        behaviour.fields[info.name] = flag;
                    }
                    break;
                }
                case FieldKind::String:
                    fill(text, std::get<std::string>(value));
                    if (ImGui::InputText(info.name.c_str(), text, sizeof text)) behaviour.fields[info.name] = std::string(text);
                    track();
                    break;
                case FieldKind::SceneNode: {
                    // An empty scene path in a scene runner's reference means its own scene.
                    // Copies: choosing an entry below may erase the stored value.
                    const NodeRef ref = std::get<NodeRef>(value);
                    const std::string refScene = ref.scene.empty() && scene >= 0 ? world_.scenes[scene].path : ref.scene;
                    std::string current = "(none)";
                    if (auto* loaded = world_.findScene(refScene))
                        if (const auto* node = loaded->scene.findNode(ref.node)) current = refScene + ": " + node->name;
                    if (ImGui::BeginCombo(info.name.c_str(), current.c_str())) {
                        if (ImGui::Selectable("(none)", ref.node == 0)) {
                            checkpoint();
                            behaviour.fields.erase(info.name);
                        }
                        for (std::size_t j = 0; j < world_.scenes.size(); ++j)
                            for (const auto& node : world_.scenes[j].scene.nodes) {
                                const auto label = world_.scenes[j].path + ": " + node.name + "##" +
                                                   std::to_string(j) + "/" + std::to_string(node.id);
                                if (ImGui::Selectable(label.c_str(), refScene == world_.scenes[j].path && ref.node == node.id)) {
                                    checkpoint();
                                    const bool own = scene == static_cast<int>(j);
                                    behaviour.fields[info.name] = NodeRef{own ? std::string() : world_.scenes[j].path, node.id};
                                }
                            }
                        ImGui::EndCombo();
                    }
                    break;
                }
                }
                if (overridden && behaviour.fields.count(info.name)) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset")) {
                        checkpoint();
                        behaviour.fields.erase(info.name);
                    }
                }
                ImGui::PopID();
            }
            if (ImGui::Button("Remove Behaviour")) remove = k;
        }
        ImGui::PopID();
    }
    if (remove) {
        checkpoint();
        runner.behaviours.erase(runner.behaviours.begin() + static_cast<std::ptrdiff_t>(*remove));
    }
    ImGui::Separator();
    if (ImGui::Button("Add Behaviour")) {
        checkpoint();
        runner.behaviours.push_back({});
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete Runner")) erase();
}

const Editor::CachedFields& Editor::fieldsOf(const std::string& script) {
    auto& entry = fieldCache_[script];
    std::error_code error;
    const auto stamp = script.empty() ? std::filesystem::file_time_type() :
                                        std::filesystem::last_write_time(project_->resolve(script), error);
    if (script.empty() || error) {
        entry = {};
        if (!script.empty()) entry.error = "Script not found";
        return entry;
    }
    if (entry.loaded && entry.stamp == stamp) return entry;
    entry.loaded = true;
    entry.stamp = stamp;
    try {
        entry.fields = ScriptRuntime::fields(project_->root, script);
        entry.error.clear();
    } catch (const std::exception& failure) {
        entry.fields.clear();
        entry.error = failure.what();
    }
    return entry;
}
}
