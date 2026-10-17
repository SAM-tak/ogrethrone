// The Scene, Runners and Inspector panels and the drill-down breadcrumb of the Editor
// (TDDja/05 6, 06 6).
#include "editor/Editor.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <optional>

namespace throne {
namespace {
/// A pending reparent, applied after the tree has been drawn.
struct Move {
    NodeId id, parent;
};
std::optional<Move> pendingMove;

/// Accepts a node of the edited file (payload @p type) dropped on the last item, to go under @p parent.
void dropTarget(const char* type, NodeId parent) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const auto* payload = ImGui::AcceptDragDropPayload(type))
        pendingMove = Move{*static_cast<const NodeId*>(payload->Data), parent};
    ImGui::EndDragDropTarget();
}

/// Copies @p text into a fixed buffer for an InputText.
template <std::size_t Size>
void fill(char (&buffer)[Size], const std::string& text) {
    std::snprintf(buffer, Size, "%s", text.c_str());
}

/// Returns @p path without its first @p count IDs.
NodePath after(const NodePath& path, std::size_t count) { return NodePath(path.begin() + count, path.end()); }
}

void Editor::breadcrumb() {
    for (std::size_t i = 0; i < contexts_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (i) {
            ImGui::SameLine();
            ImGui::TextUnformatted(">");
            ImGui::SameLine();
        }
        const auto label = i == 0 ? std::string("Level") : contexts_[i].path;
        if (ImGui::SmallButton(label.c_str()) && i + 1 < contexts_.size() && !runtime_)
            navigate_.emplace(contexts_.begin(), contexts_.begin() + static_cast<std::ptrdiff_t>(i + 1));
        ImGui::PopID();
    }
}

void Editor::scenePanel() {
    if (ImGui::Begin("Scene")) {
        ImGui::BeginDisabled(runtime_ != nullptr);
        if (!content()) {
            if (ImGui::Button("Add Scene")) addScene();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ScenePath", newScenePath_, sizeof newScenePath_);
        } else if (content()->variant()) {
            ImGui::TextColored(variantColour, "Variant of %s", content()->base.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Open Base")) enter({Context::Kind::Prefab, content()->base});
        } else {
            if (ImGui::Button("Add Node")) addNode();
            ImGui::SameLine();
            if (ImGui::Button("Add Instance")) addInstance();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##PrefabPath", newPrefabPath_, sizeof newPrefabPath_);
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (view_.instances.empty()) ImGui::TextDisabled("No scenes. Add one above.");
        std::optional<std::uint32_t> removed;
        for (std::size_t i = 0; i < view_.instances.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            std::string label = contexts_.back().path;
            if (!content()) {
                const auto* instance = doc_.world.level.findScene(view_.instances[i].id);
                label = "#" + std::to_string(view_.instances[i].id) + " " + (instance ? instance->scene : "?");
            }
            // At the level a scene instance is a row of its own, selected for its transform.
            auto flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
            if (!content() && selected_.kind == Selection::Kind::Scene && selected_.instance == static_cast<int>(i))
                flags |= ImGuiTreeNodeFlags_Selected;
            const bool open = ImGui::TreeNodeEx("##root", flags, "%s", label.c_str());
            if (!content() && ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                selected_ = {Selection::Kind::Scene, static_cast<int>(i)};
            // Double-clicking a scene instance drills into its scene file.
            if (!content() && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !runtime_)
                if (const auto* instance = doc_.world.level.findScene(view_.instances[i].id))
                    enter({Context::Kind::Scene, instance->scene});
            if (contexts_.back().kind == Context::Kind::Scene) dropTarget(nodePayload, 0);
            if (!content() && !runtime_) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) removed = view_.instances[i].id;
            }
            if (open) {
                for (const auto& error : view_.instances[i].expansion.errors)
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error.c_str());
                nodeTree(i, Expansion::npos);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (pendingMove && !runtime_ && content()) {
            auto before = doc_;
            if (content()->reparent(pendingMove->id, pendingMove->parent)) commit(std::move(before));
        }
        pendingMove.reset();
        if (removed) removeScene(*removed);
    }
    ImGui::End();
}

void Editor::nodeTree(std::size_t instance, std::size_t parent) {
    const auto& nodes = view_.instances[instance].expansion.nodes;
    for (std::size_t k = 0; k < nodes.size(); ++k) {
        const auto& expanded = nodes[k];
        if (expanded.parent != parent) continue;
        const bool leaf = std::none_of(nodes.begin(), nodes.end(), [&](const ExpandedNode& other) { return other.parent == k; });
        const bool chosen = selected_.kind == Selection::Kind::Node && selected_.instance == static_cast<int>(instance) &&
                            selected_.path == expanded.path;
        // Only the edited file's own nodes can be dragged or take children.
        const bool own = content() && expanded.path.size() == 1 && content()->findNode(expanded.path[0]);
        auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
        if (leaf) flags |= ImGuiTreeNodeFlags_Leaf;
        if (chosen) flags |= ImGuiTreeNodeFlags_Selected;
        if (!own) ImGui::PushStyleColor(ImGuiCol_Text, instanceColour);
        const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(k)), flags, "%s",
                                            expanded.node.name.c_str());
        if (!own) ImGui::PopStyleColor();
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            selected_ = {Selection::Kind::Node, static_cast<int>(instance), expanded.path};
        // Double-clicking an instance node drills into its prefab.
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !expanded.node.prefab.empty() &&
            !runtime_)
            enter({Context::Kind::Prefab, expanded.node.prefab});
        if (own && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(nodePayload, &expanded.path[0], sizeof expanded.path[0]);
            ImGui::TextUnformatted(expanded.node.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (own && expanded.node.prefab.empty()) dropTarget(nodePayload, expanded.path[0]);
        // An instance names its prefab after it, coloured by whether that is a variant.
        if (!expanded.node.prefab.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(isVariant(expanded.node.prefab) ? variantColour : instanceColour, "[%s]",
                               expanded.node.prefab.c_str());
        }
        if (open) {
            nodeTree(instance, k);
            ImGui::TreePop();
        }
    }
}

void Editor::runnersPanel() {
    if (ImGui::Begin("Runners")) {
        ImGui::BeginDisabled(runtime_ != nullptr);
        if (ImGui::Button("+ Runner")) addRunner();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(content() ? "to the edited file" : "to the level");
        const auto item = [&](const char* label, int instance, const NodePath& path, std::uint32_t id, bool own) {
            ImGui::PushID(static_cast<int>(id));
            const bool chosen = selected_.kind == Selection::Kind::Runner && selected_.instance == instance &&
                                selected_.path == path && selected_.runner == id;
            if (!own) ImGui::PushStyleColor(ImGuiCol_Text, instanceColour);
            if (ImGui::Selectable(label, chosen)) selected_ = {Selection::Kind::Runner, instance, path, id};
            if (!own) ImGui::PopStyleColor();
            ImGui::PopID();
        };
        if (!content() && ImGui::TreeNodeEx("Level", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& runner : doc_.world.level.runners) item(runner.name.c_str(), -1, {}, runner.id, true);
            ImGui::TreePop();
        }
        for (std::size_t i = 0; i < view_.instances.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const auto label = content() ? contexts_.back().path : "#" + std::to_string(view_.instances[i].id);
            if (ImGui::TreeNodeEx("##runners", ImGuiTreeNodeFlags_DefaultOpen, "%s", label.c_str())) {
                for (std::size_t r = 0; r < view_.instances[i].expansion.runners.size(); ++r) {
                    const auto& expanded = view_.instances[i].expansion.runners[r];
                    ImGui::PushID(static_cast<int>(r));
                    item(expanded.runner.name.c_str(), static_cast<int>(i), expanded.instance, expanded.runner.id,
                         content() && !content()->variant() && expanded.instance.empty());
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

void Editor::inspector() {
    if (ImGui::Begin("Inspector")) {
        ImGui::BeginDisabled(runtime_ != nullptr);
        if (const auto* node = selectedNode()) nodeInspector(*node);
        else if (const auto* runner = selectedRunner()) runnerInspector(*runner);
        else if (auto* placed = selectedScene()) sceneInspector(*placed);
        else ImGui::TextDisabled("Nothing selected");
        ImGui::EndDisabled();
    }
    ImGui::End();
}

void Editor::nodeInspector(const ExpandedNode& expanded) {
    // One undo step per widget interaction, recorded when the widget is released.
    const auto track = [this] {
        if (ImGui::IsItemActivated()) editStart_ = doc_;
        if (ImGui::IsItemDeactivatedAfterEdit()) commit(std::move(editStart_));
    };
    const auto target = targetOf(selected_.instance, selected_.path);
    NodeOverride* layer = nullptr;
    if (target.overrides)
        for (auto& entry : target.overrides->nodes)
            if (entry.path == target.relative) layer = &entry;
    ImGui::TextDisabled(target.node ? "Scene node" : "Node of an instance: edits are overrides");

    auto value = expanded.node;
    unsigned changed = 0;
    // Draws one property, coloured when overridden, with Revert next to it.
    const auto property = [&](unsigned bit, bool overridden, auto&& widget, auto&& revert) {
        if (overridden) ImGui::PushStyleColor(ImGuiCol_Text, overriddenColour);
        if (widget()) changed |= bit;
        if (overridden) ImGui::PopStyleColor();
        track();
        if (overridden) {
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(bit));
            if (ImGui::SmallButton("Revert")) {
                checkpoint();
                revert();
            }
            ImGui::PopID();
        }
    };
    constexpr auto clamp = ImGuiSliderFlags_AlwaysClamp;
    char name[256];
    fill(name, value.name);
    property(1, layer && layer->name, [&] { return ImGui::InputText("Name", name, sizeof name) && name[0] && (value.name = name, true); },
             [&] { layer->name.reset(); });
    property(2, layer && layer->position,
             [&] { return ImGui::DragFloat3("Position", value.position.data(), 0.05f, -1.0e6f, 1.0e6f, "%.3f", clamp); },
             [&] { layer->position.reset(); });
    property(4, layer && layer->rotation,
             [&] { return ImGui::DragFloat3("Rotation", value.rotation.data(), 0.5f, -1.0e6f, 1.0e6f, "%.1f", clamp); },
             [&] { layer->rotation.reset(); });
    property(8, layer && layer->scale,
             [&] { return ImGui::DragFloat3("Scale", value.scale.data(), 0.01f, 0.001f, 1.0e6f, "%.3f", clamp); },
             [&] { layer->scale.reset(); });
    property(16, layer && layer->colour, [&] { return ImGui::ColorEdit3("Colour", value.colour.data()); },
             [&] { layer->colour.reset(); });
    if (changed) writeNode(value, changed);

    if (!expanded.node.prefab.empty()) {
        ImGui::Separator();
        ImGui::Text("Instance of %s", expanded.node.prefab.c_str());
        if (ImGui::Button("Open Prefab")) enter({Context::Kind::Prefab, expanded.node.prefab});
    }
    if (target.node) {
        ImGui::Separator();
        if (ImGui::Button("Duplicate")) duplicate();
        ImGui::SameLine();
        if (ImGui::Button("Delete")) erase();
    }
}

void Editor::sceneInspector(SceneInstance& instance) {
    const auto track = [this] {
        if (ImGui::IsItemActivated()) editStart_ = doc_;
        if (ImGui::IsItemDeactivatedAfterEdit()) commit(std::move(editStart_));
    };
    ImGui::TextDisabled("Scene instance #%u", instance.id);
    ImGui::TextUnformatted(instance.scene.c_str());
    constexpr auto clamp = ImGuiSliderFlags_AlwaysClamp;
    auto& transform = instance.transform;
    ImGui::DragFloat3("Position", transform.position.data(), 0.05f, -1.0e6f, 1.0e6f, "%.3f", clamp);
    track();
    ImGui::DragFloat3("Rotation", transform.rotation.data(), 0.5f, -1.0e6f, 1.0e6f, "%.1f", clamp);
    track();
    ImGui::DragFloat3("Scale", transform.scale.data(), 0.01f, 0.001f, 1.0e6f, "%.3f", clamp);
    track();
    ImGui::Separator();
    ImGui::Text("%zu node and %zu runner overrides", instance.overrides.nodes.size(), instance.overrides.runners.size());
    if (!instance.overrides.empty() && ImGui::Button("Revert All Overrides")) {
        checkpoint();
        instance.overrides = {};
    }
    ImGui::Separator();
    if (ImGui::Button("Open Scene")) enter({Context::Kind::Scene, instance.scene});
    ImGui::SameLine();
    if (ImGui::Button("Remove")) removeScene(instance.id);
}

void Editor::runnerInspector(const Runner& shown) {
    const auto track = [this] {
        if (ImGui::IsItemActivated()) editStart_ = doc_;
        if (ImGui::IsItemDeactivatedAfterEdit()) commit(std::move(editStart_));
    };
    auto target = runnerTarget();
    ImGui::TextDisabled(target.runner ? "Runner" : "Runner of an instance: field edits are overrides");
    char text[256];
    if (target.runner) {
        fill(text, target.runner->name);
        if (ImGui::InputText("Name", text, sizeof text) && text[0]) target.runner->name = text;
        track();
    } else {
        ImGui::Text("%s", shown.name.c_str());
    }
    // The layer field values are written to: the runner itself, or this instance's overrides.
    const auto layer = [&](std::size_t k) -> std::map<std::string, FieldValue>* {
        if (target.runner) return &target.runner->behaviours[k].fields;
        if (target.overrides) return &target.overrides->fieldsOf(target.instance, selected_.runner, k).fields;
        return nullptr;
    };
    const auto overridden = [&](std::size_t k, const std::string& name) {
        if (target.runner) return target.runner->behaviours[k].fields.count(name) > 0;
        if (!target.overrides) return false;
        for (const auto& entry : target.overrides->runners)
            if (entry.instance == target.instance && entry.runner == selected_.runner && entry.behaviour == k)
                return entry.fields.count(name) > 0;
        return false;
    };

    std::optional<std::size_t> remove;
    for (std::size_t k = 0; k < shown.behaviours.size(); ++k) {
        const auto& behaviour = shown.behaviours[k];
        ImGui::PushID(static_cast<int>(k));
        const auto title = (behaviour.script.empty() ? std::string("(no script)") : behaviour.script) + "###behaviour";
        if (ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (target.runner) {
                fill(text, behaviour.script);
                if (ImGui::InputTextWithHint("Script", "scripts/....lh", text, sizeof text))
                    target.runner->behaviours[k].script = text;
                track();
            }
            const auto& cached = fieldsOf(behaviour.script);
            if (!cached.error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", cached.error.c_str());
            for (const auto& info : cached.fields) {
                ImGui::PushID(info.name.c_str());
                const auto found = behaviour.fields.find(info.name);
                // A stored value of the wrong type (the script changed) shows the default.
                const FieldValue value = found != behaviour.fields.end() && found->second.index() == info.defaultValue.index()
                                             ? found->second
                                             : info.defaultValue;
                const bool marked = overridden(k, info.name);
                if (marked && !target.runner) ImGui::PushStyleColor(ImGuiCol_Text, overriddenColour);
                switch (info.kind) {
                case FieldKind::Number: {
                    auto number = static_cast<float>(std::get<double>(value));
                    if (ImGui::DragFloat(info.name.c_str(), &number, 0.1f))
                        if (auto* fields = layer(k)) (*fields)[info.name] = double(number);
                    track();
                    break;
                }
                case FieldKind::Bool: {
                    bool flag = std::get<bool>(value);
                    if (ImGui::Checkbox(info.name.c_str(), &flag)) {
                        checkpoint();
                        if (auto* fields = layer(k)) (*fields)[info.name] = flag;
                    }
                    break;
                }
                case FieldKind::String:
                    fill(text, std::get<std::string>(value));
                    if (ImGui::InputText(info.name.c_str(), text, sizeof text))
                        if (auto* fields = layer(k)) (*fields)[info.name] = std::string(text);
                    track();
                    break;
                case FieldKind::SceneNode: {
                    // A reference without a scene is relative to where the runner is; one
                    // with a scene instance ID (level runners) starts at that scene.
                    const NodeRef ref = std::get<NodeRef>(value);
                    int refInstance = selected_.instance;
                    NodePath refPath = selected_.instance >= 0 ? selected_.path : NodePath{};
                    if (ref.scene) {
                        refInstance = -1;
                        refPath.clear();
                        for (std::size_t i = 0; i < view_.instances.size(); ++i)
                            if (view_.instances[i].id == ref.scene) refInstance = static_cast<int>(i);
                    }
                    refPath.push_back(ref.node);
                    refPath.insert(refPath.end(), ref.inner.begin(), ref.inner.end());
                    std::string current = "(none)";
                    auto refAt = Expansion::npos;
                    if (ref.node && refInstance >= 0)
                        if (refAt = view_.instances[refInstance].expansion.find(refPath); refAt != Expansion::npos)
                            current = view_.instances[refInstance].expansion.nodes[refAt].node.name;
                    if (ImGui::BeginCombo(info.name.c_str(), current.c_str())) {
                        if (ImGui::Selectable("(none)", ref.node == 0)) {
                            checkpoint();
                            if (auto* fields = layer(k)) (*fields)[info.name] = NodeRef{};
                        }
                        // A level runner may pick any node; any other runner, nodes of its own instance.
                        const auto& prefix = selected_.instance >= 0 ? selected_.path : NodePath{};
                        for (std::size_t i = 0; i < view_.instances.size(); ++i) {
                            if (selected_.instance >= 0 && static_cast<int>(i) != selected_.instance) continue;
                            const auto& nodes = view_.instances[i].expansion.nodes;
                            for (std::size_t n = 0; n < nodes.size(); ++n) {
                                const auto& node = nodes[n];
                                if (node.path.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), node.path.begin()))
                                    continue;
                                std::string label = node.node.name;
                                if (selected_.instance < 0) label = "#" + std::to_string(view_.instances[i].id) + " " + label;
                                label += "##" + std::to_string(i);
                                for (const auto step : node.path) label += "/" + std::to_string(step);
                                if (!ImGui::Selectable(label.c_str(), static_cast<int>(i) == refInstance && n == refAt))
                                    continue;
                                checkpoint();
                                // The instance's own root is named by the root's ID inside the prefab.
                                auto relative = after(node.path, prefix.size());
                                if (relative.empty()) relative = {node.root};
                                NodeRef picked{selected_.instance < 0 ? view_.instances[i].id : 0, relative[0], after(relative, 1)};
                                if (auto* fields = layer(k)) (*fields)[info.name] = picked;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    break;
                }
                }
                if (marked && !target.runner) ImGui::PopStyleColor();
                if (marked) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(target.runner ? "Reset" : "Revert")) {
                        checkpoint();
                        if (auto* fields = layer(k)) fields->erase(info.name);
                    }
                }
                ImGui::PopID();
            }
            if (target.runner && ImGui::Button("Remove Behaviour")) remove = k;
        }
        ImGui::PopID();
    }
    if (!target.runner) return;
    if (remove) {
        checkpoint();
        target.runner->behaviours.erase(target.runner->behaviours.begin() + static_cast<std::ptrdiff_t>(*remove));
    }
    ImGui::Separator();
    if (ImGui::Button("Add Behaviour")) {
        checkpoint();
        target.runner->behaviours.push_back({});
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
