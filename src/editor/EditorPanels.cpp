// The Scene, Runners and Inspector panels and the drill-down breadcrumb of the Editor
// (TDDja/05 6, 06 6).
#include "editor/Editor.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstdio>
#include <optional>
#include <string_view>

namespace throne {
namespace {
/// A pending reparent in the file view instance @p instance shows, applied after
/// the tree has been drawn: @p id goes under @p parent, before its child @p before (0: last).
struct Move {
    int instance;
    NodeId id, parent, before = 0;
};
std::optional<Move> pendingMove;

/// Accepts a node (payload @p type, a DragNode) dropped on the last item, to go
/// under @p parent; only a node of the same view instance, so of the same file.
void dropTarget(const char* type, int instance, NodeId parent) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const auto* payload = ImGui::AcceptDragDropPayload(type)) {
        const auto& dragged = *static_cast<const DragNode*>(payload->Data);
        if (dragged.instance == instance) pendingMove = Move{instance, dragged.id, parent};
    }
    ImGui::EndDragDropTarget();
}

/// Where a node dropped between rows goes: under @p parent before @p before,
/// shown by a line starting at @p x (the level's indentation).
struct Slot {
    NodeId parent, before;
    float x;
};

/// Accepts a node dropped on @p rect into one of @p slots, deepest first: the
/// mouse picks the deepest one whose indentation it is right of (else the last).
/// While hovering, draws a line at @p y where it would go.
void siblingTarget(const char* type, int instance, const ImRect& rect, const char* id, const std::vector<Slot>& slots,
                   float y) {
    if (!ImGui::BeginDragDropTargetCustom(rect, ImGui::GetID(id))) return;
    if (const auto* payload = ImGui::AcceptDragDropPayload(
            type, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
        const float mouse = ImGui::GetIO().MousePos.x, half = ImGui::GetStyle().IndentSpacing * 0.5f;
        const auto slot = std::find_if(slots.begin(), slots.end() - 1, [&](const Slot& each) { return mouse >= each.x - half; });
        ImGui::GetWindowDrawList()->AddLine({slot->x, y}, {rect.Max.x, y}, ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2);
        const auto& dragged = *static_cast<const DragNode*>(payload->Data);
        if (payload->IsDelivery() && dragged.instance == instance)
            pendingMove = Move{instance, dragged.id, slot->parent, slot->before};
    }
    ImGui::EndDragDropTarget();
}

/// Copies @p text into a fixed buffer for an InputText.
template <std::size_t Size>
void fill(char (&buffer)[Size], const std::string& text) {
    std::snprintf(buffer, Size, "%s", text.c_str());
}

/// The name of the file at @p path: its file name up to the first dot.
std::string nameOf(std::string_view path) {
    const auto file = path.substr(path.find_last_of('/') + 1);
    return std::string(file.substr(0, file.find('.')));
}

/// Returns @p path without its first @p count IDs.
NodePath after(const NodePath& path, std::size_t count) { return NodePath(path.begin() + count, path.end()); }
}

std::string Editor::instanceLabel(std::size_t instance) {
    if (content()) {
        const auto kind = contexts_.back().kind == Context::Kind::Scene ? " [Scene]" : " [Prefab]";
        return nameOf(contexts_.back().path) + kind;
    }
    const auto* placed = doc_.world.level.findScene(view_.instances[instance].id);
    if (!placed) return "? [Scene]";
    auto label = nameOf(placed->scene) + " [Scene]";
    // The same scene placed more than once is told apart by the instance's ID.
    const auto& scenes = doc_.world.level.scenes;
    if (std::count_if(scenes.begin(), scenes.end(), [&](const SceneInstance& each) { return each.scene == placed->scene; }) > 1)
        label += " #" + std::to_string(placed->id);
    return label;
}

void Editor::breadcrumb() {
    for (std::size_t i = 0; i < contexts_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (i) {
            ImGui::SameLine();
            ImGui::TextUnformatted(">");
            ImGui::SameLine();
        }
        const auto label = i == 0 ? (levelPath_[0] ? nameOf(levelPath_) : std::string("untitled")) + " [Level]"
                                  : nameOf(contexts_[i].path) +
                                        (contexts_[i].kind == Context::Kind::Scene ? " [Scene]" : " [Prefab]");
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
            if (!view_.instances.empty()) removedList(content()->overrides, view_.instances[0].expansion, {});
        }
        // Into an instance (at the level, a selected scene instance or a node in one)
        // these add overrides; see insert().
        if (ImGui::Button("Add Node")) addNode();
        ImGui::SameLine();
        if (ImGui::Button("Add Instance")) addInstance();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##PrefabPath", newPrefabPath_, sizeof newPrefabPath_);
        ImGui::EndDisabled();
        ImGui::Separator();
        if (view_.instances.empty()) ImGui::TextDisabled("No scenes. Add one above.");
        std::optional<std::uint32_t> removed;
        for (std::size_t i = 0; i < view_.instances.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const auto label = instanceLabel(i);
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
            if (contexts_.back().kind != Context::Kind::Prefab && fileOf(static_cast<int>(i)))
                dropTarget(nodePayload, static_cast<int>(i), 0);
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
        if (auto* file = pendingMove && !runtime_ ? fileOf(pendingMove->instance) : nullptr) {
            auto before = doc_;
            if (file->reparent(pendingMove->id, pendingMove->parent, pendingMove->before)) commit(std::move(before));
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
        auto* file = fileOf(static_cast<int>(instance));
        const bool own = file && expanded.path.size() == 1 && file->findNode(expanded.path[0]);
        auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
        if (leaf) flags |= ImGuiTreeNodeFlags_Leaf;
        if (chosen) flags |= ImGuiTreeNodeFlags_Selected;
        // Nodes from instances are blue; those an instance added are overrides.
        if (!own) ImGui::PushStyleColor(ImGuiCol_Text, expanded.added ? overriddenColour : instanceColour);
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
            const DragNode dragged{static_cast<int>(instance), expanded.path[0]};
            ImGui::SetDragDropPayload(nodePayload, &dragged, sizeof dragged);
            ImGui::TextUnformatted(expanded.node.name.c_str());
            ImGui::EndDragDropSource();
        }
        // The top and bottom quarters of a row put a dropped node beside it, the rest
        // inside it (smaller drop targets win). A prefab keeps its single root.
        const auto movable = [&](std::size_t at) {
            const auto& each = nodes[at];
            return file && each.path.size() == 1 && file->findNode(each.path[0]) &&
                   !(contexts_.back().kind == Context::Kind::Prefab && each.node.parent == 0);
        };
        if (movable(k)) {
            const ImRect row(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            const float edge = row.GetHeight() * 0.25f, indent = ImGui::GetStyle().IndentSpacing;
            // Below the last child, moving left goes out a level: after the parent, and so on.
            std::vector<Slot> below;
            for (auto at = k;;) {
                auto next = at + 1;
                while (next < nodes.size() && nodes[next].parent != nodes[at].parent) ++next;
                below.push_back({nodes[at].node.parent, next < nodes.size() ? nodes[next].node.id : 0,
                                 row.Min.x - indent * static_cast<float>(below.size())});
                if (next < nodes.size() || nodes[at].parent == Expansion::npos || !movable(nodes[at].parent)) break;
                at = nodes[at].parent;
            }
            const float left = ImGui::GetCurrentWindow()->WorkRect.Min.x;
            siblingTarget(nodePayload, static_cast<int>(instance), ImRect({left, row.Min.y}, {row.Max.x, row.Min.y + edge}),
                          ("above" + std::to_string(k)).c_str(), {{expanded.node.parent, expanded.node.id, row.Min.x}}, row.Min.y);
            siblingTarget(nodePayload, static_cast<int>(instance), ImRect({left, row.Max.y - edge}, row.Max), ("below" + std::to_string(k)).c_str(), below,
                          row.Max.y);
        }
        if (own && expanded.node.prefab.empty()) dropTarget(nodePayload, static_cast<int>(instance), expanded.path[0]);
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
        auto& level = doc_.world.level;
        bool serial = level.serialExecution;
        if (ImGui::Checkbox("Run engine group serially", &serial)) checkpoint(), level.serialExecution = serial;
        static char newStage[256] = "LateUpdate";
        ImGui::InputText("Stage name", newStage, sizeof newStage);
        if (ImGui::Button("Add Stage") && newStage[0]) {
            const auto stages = ScriptRuntime::schedule(doc_.world, view_);
            if (std::none_of(stages.begin(), stages.end(), [&](const ExecutionStage& stage) { return stage.name == newStage || std::find(stage.parallel.begin(), stage.parallel.end(), newStage) != stage.parallel.end(); })) {
                checkpoint();
                const auto at = std::find_if(level.stages.begin(), level.stages.end(), [](const ExecutionStage& stage) {
                    return stage.name == "SyncRender";
                });
                level.stages.insert(at, {newStage, {}});
            }
        }
        if (ImGui::Button("+ Runner Behaviour")) addRunner();
        const auto stages = ScriptRuntime::schedule(doc_.world, view_);
        // Display names can repeat; keep row identity tied to the full instance path.
        const auto rowId = [](const char* kind, int instance, const NodePath& path, std::uint64_t item) {
            auto id = std::string(kind) + "/" + std::to_string(instance);
            for (const auto step : path) id += "/" + std::to_string(step);
            return id + "/" + std::to_string(item);
        };
        const auto direct = [&](const Runner& runner, int instance, const NodePath& path) {
            ImGui::PushID(rowId("runner", instance, path, runner.id).c_str());
            const auto label = runner.name + " [Runner]###runner";
            if (ImGui::Selectable(label.c_str())) selected_ = {Selection::Kind::Runner, instance, path, runner.id};
            for (const auto& behaviour : runner.behaviours)
                ImGui::BulletText("%s [%s]", behaviour.script.c_str(), behaviour.system ? "System" : "Behaviour");
            ImGui::PopID();
        };
        for (const auto& stage : stages) {
            const bool engine = !stage.parallel.empty() || stage.name == "FetchInput" || stage.name == "ApplyPhysics" ||
                stage.name == "SyncRender" || stage.name == "SyncSimulation";
            const auto label = stage.name + (engine ? " [System]" : " [Runner]");
            ImGui::PushID(stage.name.c_str());
            const bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            if (!engine) {
                const auto index = static_cast<std::size_t>(&stage - stages.data());
                ImGui::SameLine();
                if (ImGui::SmallButton("Up") && index > 1) {
                    checkpoint();
                    level.stages = stages;
                    std::swap(level.stages[index], level.stages[index - 1]);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Down") && index + 1 < stages.size()) {
                    checkpoint();
                    level.stages = stages;
                    std::swap(level.stages[index], level.stages[index + 1]);
                }
                bool used = stage.name == "Update";
                for (const auto& runner : level.runners) used |= runner.name == stage.name;
                for (const auto& instance : view_.instances) {
                    for (const auto& runner : instance.expansion.runners) used |= runner.runner.name == stage.name;
                    for (const auto& node : instance.expansion.nodes)
                        for (const auto& behaviour : node.node.behaviours) used |= behaviour.runner == stage.name;
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(used);
                if (ImGui::SmallButton("Remove Stage")) {
                    checkpoint();
                    level.stages = stages;
                    level.stages.erase(level.stages.begin() + static_cast<std::ptrdiff_t>(index));
                }
                ImGui::EndDisabled();
            }
            if (open) {
                for (const auto& member : stage.parallel) ImGui::BulletText("%s [System]", member.c_str());
                // Direct attachments precede node attachments, just as at runtime.
                for (std::size_t i = 0; i < view_.instances.size(); ++i)
                    for (const auto& runner : view_.instances[i].expansion.runners)
                        if (runner.runner.name == stage.name) direct(runner.runner, static_cast<int>(i), runner.instance);
                for (const auto& runner : level.runners)
                    if (runner.name == stage.name) direct(runner, -1, {});
                for (std::size_t i = 0; i < view_.instances.size(); ++i)
                    for (const auto n : view_.instances[i].expansion.hierarchy()) {
                        const auto& node = view_.instances[i].expansion.nodes[n];
                        for (std::size_t k = 0; k < node.node.behaviours.size(); ++k) {
                            const auto& behaviour = node.node.behaviours[k];
                            if (behaviour.runner != stage.name) continue;
                            ImGui::PushID(rowId("node", static_cast<int>(i), node.path, k).c_str());
                            const auto text = instanceLabel(i) + " / " + node.node.name + " / " + behaviour.script + "###attachment";
                            if (ImGui::Selectable(text.c_str()))
                                selected_ = {Selection::Kind::Node, static_cast<int>(i), node.path};
                            ImGui::PopID();
                        }
                    }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        const auto invalid = [&](const Runner& runner) {
            return std::none_of(stages.begin(), stages.end(), [&](const ExecutionStage& stage) {
                return stage.name == runner.name;
            });
        };
        for (std::size_t i = 0; i < view_.instances.size(); ++i)
            for (const auto& runner : view_.instances[i].expansion.runners)
                if (invalid(runner.runner)) direct(runner.runner, static_cast<int>(i), runner.instance);
        for (const auto& runner : level.runners)
            if (invalid(runner)) direct(runner, -1, {});
        ImGui::EndDisabled();
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
    ImGui::TextDisabled(target.addedTo ? "Added by an instance: kept in its overrides"
                        : target.node  ? "Scene node"
                                       : "Node of an instance: edits are overrides");

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
    // Components, one section each. A plain node the edited file holds can add and
    // remove them; elsewhere only their properties change (as overrides).
    auto* own = target.node && target.node->prefab.empty() ? target.node : nullptr;
    bool keepTransform = true, keepBox = true;
    constexpr auto open = ImGuiTreeNodeFlags_DefaultOpen;
    if (auto& transform = value.transform;
        transform && ImGui::CollapsingHeader("Transform", own ? &keepTransform : nullptr, open)) {
        property(2, layer && layer->position,
                 [&] { return ImGui::DragFloat3("Position", transform->position.data(), 0.05f, -1.0e6f, 1.0e6f, "%.3f", clamp); },
                 [&] { layer->position.reset(); });
        property(4, layer && layer->rotation,
                 [&] { return ImGui::DragFloat3("Rotation", transform->rotation.data(), 0.5f, -1.0e6f, 1.0e6f, "%.1f", clamp); },
                 [&] { layer->rotation.reset(); });
        property(8, layer && layer->scale,
                 [&] { return ImGui::DragFloat3("Scale", transform->scale.data(), 0.01f, 0.001f, 1.0e6f, "%.3f", clamp); },
                 [&] { layer->scale.reset(); });
    }
    if (auto& box = value.box; box && ImGui::CollapsingHeader("Box Primitive", own ? &keepBox : nullptr, open))
        property(16, layer && layer->colour, [&] { return ImGui::ColorEdit3("Colour", box->colour.data()); },
                 [&] { layer->colour.reset(); });
    if (changed) writeNode(value, changed);
    if (own && (!keepTransform || !keepBox)) {
        checkpoint();
        if (!keepTransform) own->transform.reset();
        if (!keepBox) own->box.reset();
    }
    const auto scalar = [&](const char* label, float& number, float low = 0.001f, float high = 1.0e6f) {
        const bool edited=ImGui::DragFloat(label,&number,.05f,low,high,"%.3f",clamp);
        track(); return edited;
    };
    const auto vector = [&](const char* label, Vec3& number, float low = -1.0e6f) {
        const bool edited=ImGui::DragFloat3(label,number.data(),.05f,low,1.0e6f,"%.3f",clamp);
        track(); return edited;
    };
    const auto colour = [&](auto& c) {
        const bool edited=ImGui::ColorEdit3("Colour",c.colour.data()); track(); return edited;
    };
    const auto collider = [&](auto& c) {
        bool edited=vector("Centre",c.centre);
        edited|=scalar("Density (kg/m3)",c.density);
        edited|=scalar("Friction",c.friction,0,1);
        edited|=scalar("Restitution",c.restitution,0,1);
        edited|=ImGui::Checkbox("Sensor",&c.sensor); track(); return edited;
    };
    // Components inherited from a prefab permit value overrides, but not removal.
    const auto component = [&](const char* label, auto& shown, auto member, auto overrideMember, auto fields) {
        if (!shown) return;
        ImGui::PushID(label);
        bool keep=true;
        if (ImGui::CollapsingHeader(label,own?&keep:nullptr,open) && fields(*shown)) {
            if (own) own->*member=shown;
            else if(target.overrides) target.overrides->node(target.relative).*overrideMember=shown;
        }
        if (!keep && own) { checkpoint(); (own->*member).reset(); }
        if (layer && layer->*overrideMember && ImGui::Button("Revert Component")) {
            checkpoint(); (layer->*overrideMember).reset();
        }
        ImGui::PopID();
    };
    const auto texturePath=[&](const char* label,std::string& path) {
        char text[256]; fill(text,path); bool edited=ImGui::InputText(label,text,sizeof text); track();
        if(edited) path=text; return edited;
    };
    component("Model",value.model,&SceneNode::model,&NodeOverride::model,[&](auto& c) {
        bool edited=texturePath("File",c.path);
        edited|=ImGui::Checkbox("DirectX normal maps",&c.directXNormals); track();
        ImGui::TextDisabled("A project-relative FBX file: its meshes and PBS materials.");
        return edited;
    });
    component("Particle System",value.particles,&SceneNode::particles,&NodeOverride::particles,[&](auto& c) {
        bool edited=texturePath("Template",c.definition);
        edited|=texturePath("Particle Script",c.script);
        edited|=ImGui::Checkbox("Enabled",&c.enabled); track();
        ImGui::TextDisabled("Empty script uses OGRE's built-in ParticleFX2 examples.");
        return edited;
    });
    component("PBS Material",value.material,&SceneNode::material,&NodeOverride::material,[&](auto& c) {
        bool edited=scalar("Roughness",c.roughness,.02f,1);
        bool metallic=c.metalness>=0;
        if(ImGui::Checkbox("Metallic Workflow",&metallic)) { c.metalness=metallic?0.0f:-1.0f; edited=true; } track();
        if(metallic) edited|=scalar("Metalness",c.metalness,0,1);
        else { edited|=ImGui::ColorEdit3("Fresnel F0",c.fresnel.data()); track(); }
        edited|=ImGui::ColorEdit3("Emissive",c.emissive.data(),ImGuiColorEditFlags_HDR|ImGuiColorEditFlags_Float); track();
        edited|=scalar("UV Scale",c.uvScale,.001f,10000);
        edited|=texturePath("Diffuse Map",c.diffuseMap); edited|=texturePath("Normal Map",c.normalMap);
        edited|=texturePath("Roughness Map",c.roughnessMap); edited|=texturePath("Specular Map",c.specularMap);
        edited|=texturePath("Reflection Cubemap",c.reflectionMap); return edited;
    });
    component("Light",value.light,&SceneNode::light,&NodeOverride::light,[&](auto& c) {
        int type=static_cast<int>(c.type);
        bool edited=ImGui::Combo("Type",&type,"Directional\0Point\0Spot\0Area Approx\0Area LTC\0"); track();
        c.type=static_cast<LightType>(type);
        edited|=ImGui::Checkbox("Enabled",&c.enabled); track(); edited|=colour(c);
        edited|=scalar("Power",c.power,0);
        auto direction=c.direction;
        if(vector("Direction (local)",direction) && direction.x*direction.x+direction.y*direction.y+direction.z*direction.z>1e-10f) { c.direction=direction; edited=true; }
        if(c.type!=LightType::Directional) {
            edited|=scalar("Radius",c.radius); edited|=scalar("Falloff Threshold",c.threshold,.00001f,.999f);
        }
        if(c.type==LightType::Point || c.type==LightType::Spot) {
            edited|=ImGui::Checkbox("Cast Shadows",&c.castShadows); track();
        }
        if(c.type==LightType::Spot) {
            edited|=scalar("Inner Angle",c.innerAngle,.01f,179);
            c.outerAngle=(std::max)(c.outerAngle,c.innerAngle);
            edited|=scalar("Outer Angle",c.outerAngle,c.innerAngle,179);
        }
        if(c.type==LightType::AreaApprox || c.type==LightType::AreaLtc) {
            edited|=scalar("Width",c.width); edited|=scalar("Height",c.height);
            edited|=ImGui::Checkbox("Two Sided",&c.twoSided); track();
            edited|=ImGui::Checkbox("Show Emitter",&c.showEmitter); track();
            edited|=texturePath("Approx Mask",c.mask);
            ImGui::TextDisabled("Area lights use Forward; shadows are not generated.");
        }
        return edited;
    });
    component("Sphere Primitive",value.sphere,&SceneNode::sphere,&NodeOverride::sphere,[&](auto& c) {
        bool edited=colour(c); edited|=scalar("Radius",c.radius); return edited;
    });
    component("Capsule Primitive",value.capsule,&SceneNode::capsule,&NodeOverride::capsule,[&](auto& c) {
        bool edited=colour(c); edited|=scalar("Radius",c.radius);
        c.height=(std::max)(c.height,2*c.radius); edited|=scalar("Height",c.height,2*c.radius); return edited;
    });
    component("Plane Primitive",value.plane,&SceneNode::plane,&NodeOverride::plane,[&](auto& c) {
        bool edited=colour(c); edited|=scalar("Width",c.width); edited|=scalar("Depth",c.depth); return edited;
    });
    component("Box Collider",value.boxCollider,&SceneNode::boxCollider,&NodeOverride::boxCollider,[&](auto& c) {
        bool edited=collider(c); edited|=vector("Size",c.size,.001f); return edited;
    });
    component("Sphere Collider",value.sphereCollider,&SceneNode::sphereCollider,&NodeOverride::sphereCollider,[&](auto& c) {
        bool edited=collider(c); edited|=scalar("Radius",c.radius); return edited;
    });
    component("Capsule Collider",value.capsuleCollider,&SceneNode::capsuleCollider,&NodeOverride::capsuleCollider,[&](auto& c) {
        bool edited=collider(c); edited|=scalar("Radius",c.radius);
        c.height=(std::max)(c.height,2*c.radius); edited|=scalar("Height",c.height,2*c.radius); return edited;
    });
    component("Plane Collider",value.planeCollider,&SceneNode::planeCollider,&NodeOverride::planeCollider,[&](auto& c) {
        bool edited=collider(c); edited|=scalar("Width",c.width); edited|=scalar("Depth",c.depth); return edited;
    });
    component("RigidBody",value.rigidBody,&SceneNode::rigidBody,&NodeOverride::rigidBody,[&](auto& c) {
        int type=static_cast<int>(c.type);
        bool edited=ImGui::Combo("Motion Type",&type,"Static\0Kinematic\0Dynamic\0"); track();
        c.type=static_cast<BodyType>(type);
        edited|=vector("Linear Velocity",c.linearVelocity);
        edited|=vector("Angular Velocity (deg/s)",c.angularVelocity);
        edited|=scalar("Gravity Scale",c.gravityScale,-1000,1000);
        edited|=scalar("Linear Damping",c.linearDamping,0);
        edited|=scalar("Angular Damping",c.angularDamping,0);
        edited|=ImGui::Checkbox("Bullet (CCD)",&c.bullet); track();
        if(value.planeCollider && c.type==BodyType::Dynamic) ImGui::TextWrapped("PlaneCollider requires Static or Kinematic motion.");
        return edited;
    });
    if (own && ImGui::BeginCombo("##add", "Add Component")) {
        if (!own->transform && ImGui::Selectable("Transform")) checkpoint(), own->transform.emplace();
        const auto addPrimitive=[&](const char* label,PrimitiveKind kind) {
            if (primitiveShape(*own).kind!=kind && ImGui::Selectable(label)) checkpoint(),setPrimitive(*own,kind);
        };
        addPrimitive("Box Primitive",PrimitiveKind::Box); addPrimitive("Sphere Primitive",PrimitiveKind::Sphere);
        addPrimitive("Capsule Primitive",PrimitiveKind::Capsule); addPrimitive("Plane Primitive",PrimitiveKind::Plane);
        if(!own->particles && ImGui::Selectable("Particle System")) checkpoint(),own->particles.emplace();
        if(!own->model && ImGui::Selectable("Model")) checkpoint(),own->model.emplace();
        if(!own->material && ImGui::Selectable("PBS Material")) checkpoint(),own->material.emplace();
        if(!own->light && ImGui::Selectable("Light")) checkpoint(),own->light.emplace();
        if(!own->boxCollider && ImGui::Selectable("Box Collider")) checkpoint(),own->boxCollider.emplace();
        if(!own->sphereCollider && ImGui::Selectable("Sphere Collider")) checkpoint(),own->sphereCollider.emplace();
        if(!own->capsuleCollider && ImGui::Selectable("Capsule Collider")) checkpoint(),own->capsuleCollider.emplace();
        if(!own->planeCollider && ImGui::Selectable("Plane Collider")) checkpoint(),own->planeCollider.emplace();
        if(!own->rigidBody && ImGui::Selectable("RigidBody")) {
            checkpoint(); own->rigidBody.emplace();
            if(own->planeCollider) own->rigidBody->type=BodyType::Static;
        }
        ImGui::EndCombo();
    }

    const auto editAttachment = [&](Behaviour& behaviour, bool hasRunner) {
        bool changed = false;
            char buffer[512];
            fill(buffer, behaviour.script);
            ImGui::BeginDisabled(!hasRunner && !own);
            changed |= ImGui::InputText("Script", buffer, sizeof buffer);
            ImGui::EndDisabled();
            behaviour.script = buffer;
            track();
            if (hasRunner) {
            fill(buffer, behaviour.runner);
            changed |= ImGui::InputText("Runner", buffer, sizeof buffer);
            behaviour.runner = buffer;
            track();
            }
            const auto& cached = fieldsOf(behaviour.script, hasRunner ? "Behaviour" : "Component");
            if (!cached.error.empty()) ImGui::TextWrapped("%s", cached.error.c_str());
            for (const auto& info : cached.fields) {
                ImGui::PushID(info.name.c_str());
                auto found = behaviour.fields.find(info.name);
                FieldValue field = found != behaviour.fields.end() && found->second.index() == info.defaultValue.index()
                    ? found->second : info.defaultValue;
                bool edited = false;
                if (info.kind == FieldKind::Number) {
                    float number = static_cast<float>(std::get<double>(field));
                    edited = ImGui::DragFloat(info.name.c_str(), &number, 0.1f);
                    field = double(number);
                } else if (info.kind == FieldKind::Bool) {
                    bool flag = std::get<bool>(field);
                    edited = ImGui::Checkbox(info.name.c_str(), &flag);
                    field = flag;
                } else if (info.kind == FieldKind::String) {
                    fill(buffer, std::get<std::string>(field));
                    edited = ImGui::InputText(info.name.c_str(), buffer, sizeof buffer);
                    field = std::string(buffer);
                } else if (ImGui::BeginCombo(info.name.c_str(), "Select node")) {
                    if (ImGui::Selectable("(none)")) field = NodeRef{}, edited = true;
                    for (std::size_t i = 0; i < view_.instances.size(); ++i)
                        for (const auto& node : view_.instances[i].expansion.nodes) {
                            ImGui::PushID(static_cast<int>(i));
                            const auto label = node.node.name + "##" + std::to_string(node.node.id);
                            if (ImGui::Selectable(label.c_str()) && !node.path.empty()) {
                                auto base = expanded.path;
                                if (!base.empty() && !expanded.root) base.pop_back();
                                auto path = node.path;
                                std::uint32_t scene = view_.instances[i].id;
                                if (static_cast<int>(i) == selected_.instance && path.size() >= base.size() &&
                                    std::equal(base.begin(), base.end(), path.begin())) {
                                    path = after(path, base.size());
                                    if (path.empty()) path = {node.root};
                                    scene = 0;
                                } else if (!scene) continue;
                                field = NodeRef{scene, path.front(), after(path, 1)};
                                edited = true;
                            }
                            ImGui::PopID();
                        }
                    ImGui::EndCombo();
                }
                track();
                if (edited) behaviour.fields[info.name] = field, changed = true;
                if (found != behaviour.fields.end()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset")) {
                        checkpoint();
                        behaviour.fields.erase(info.name);
                        changed = true;
                    }
                }
                ImGui::PopID();
            }
        return changed;
    };
    for (auto& [name, stored] : value.scriptComponents) {
        ImGui::PushID(name.c_str());
        bool keep = true;
        if (ImGui::CollapsingHeader((name + " [Component]").c_str(), own ? &keep : nullptr, open)) {
            auto component = stored;
            if (editAttachment(component, false)) {
                if (own) own->scriptComponents[name] = std::move(component);
                else if (target.overrides) target.overrides->node(target.relative).scriptFields[name] = component.fields;
            }
        }
        if (!keep && own) checkpoint(), own->scriptComponents.erase(name);
        if (layer && layer->scriptFields.count(name) && ImGui::Button("Revert Component Fields")) {
            checkpoint();
            layer->scriptFields.erase(name);
        }
        ImGui::PopID();
    }
    if (own) {
        static char componentName[256] = "Spin", componentScript[512] = "scripts/spin-component.lh";
        ImGui::InputText("Component name", componentName, sizeof componentName);
        ImGui::InputText("Component script", componentScript, sizeof componentScript);
        if (ImGui::Button("Add Script Component") && componentName[0] && componentScript[0] &&
            std::string_view(componentName) != "Transform" && std::string_view(componentName) != "BoxPrimitive" &&
            !own->scriptComponents.count(componentName)) {
            checkpoint();
            own->scriptComponents[componentName] = {componentScript, {}};
        }
    }

    // Work on a copy so edits never invalidate the expansion being displayed.
    auto attachments = value.behaviours;
    bool attachmentsChanged = false;
    for (std::size_t k = 0; k < attachments.size(); ++k) {
        auto& behaviour = attachments[k];
        ImGui::PushID(static_cast<int>(k));
        if (ImGui::CollapsingHeader((behaviour.script + "###nodeBehaviour").c_str(), open)) {
            attachmentsChanged |= editAttachment(behaviour, true);
            if (ImGui::Button("Remove Behaviour")) {
                checkpoint();
                attachments.erase(attachments.begin() + static_cast<std::ptrdiff_t>(k));
                attachmentsChanged = true;
                ImGui::PopID();
                break;
            }
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Add Behaviour")) {
        checkpoint();
        attachments.push_back({});
        attachmentsChanged = true;
    }
    if (attachmentsChanged) {
        if (own) own->behaviours = std::move(attachments);
        else if (target.overrides) target.overrides->node(target.relative).behaviours = std::move(attachments);
    }
    if (layer && layer->behaviours && ImGui::Button("Revert Behaviours")) {
        checkpoint();
        layer->behaviours.reset();
    }

    // The runner fields that refer to this node; clicking one selects its runner.
    const auto& nodes = view_.instances[selected_.instance].expansion.nodes;
    if (const auto found = references_.find({static_cast<std::size_t>(selected_.instance), &expanded - nodes.data()});
        found != references_.end()) {
        ImGui::Separator();
        ImGui::TextUnformatted("Referenced by");
        for (std::size_t i = 0; i < found->second.size(); ++i) {
            const auto referrer = found->second[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(referrer.label.c_str()))
                selected_ = {referrer.runner ? Selection::Kind::Runner : Selection::Kind::Node,
                             referrer.instance, referrer.from, referrer.runner};
            ImGui::PopID();
        }
    }

    if (!expanded.node.prefab.empty()) {
        ImGui::Separator();
        ImGui::Text("Instance of %s", expanded.node.prefab.c_str());
        if (ImGui::Button("Open Prefab")) enter({Context::Kind::Prefab, expanded.node.prefab});
        // Only an instance the edited file holds itself can be unpacked into it.
        if (target.node && !target.addedTo) {
            ImGui::SameLine();
            if (ImGui::Button("Unpack")) unpack(selected_.instance, target.node->id);
            // Writes every override of this instance into the prefab (as dropping it on the file does).
            ImGui::SameLine();
            ImGui::BeginDisabled(target.node->overrides.empty());
            if (ImGui::Button("Apply")) applyOverrides(selected_.instance, target.node->id);
            ImGui::EndDisabled();
        }
        if (target.node && target.overrides)
            removedList(*target.overrides, view_.instances[selected_.instance].expansion, selected_.path);
    }
    if (target.node) {
        ImGui::Separator();
        if (!target.addedTo && ImGui::Button("Duplicate")) duplicate();
        if (!target.addedTo) ImGui::SameLine();
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
    ImGui::Separator();
    if (ImGui::Button("Open Scene")) enter({Context::Kind::Scene, instance.scene});
    ImGui::SameLine();
    if (ImGui::Button("Remove")) removeScene(instance.id);
}

void Editor::removedList(Overrides& overrides, const Expansion& expansion, const NodePath& prefix) {
    std::optional<std::size_t> restored;
    for (std::size_t i = 0; i < overrides.removed.size(); ++i) {
        auto where = prefix;
        where.insert(where.end(), overrides.removed[i].begin(), overrides.removed[i].end());
        const auto name = expansion.removed.find(where);
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextColored(overriddenColour, "Removed %s", name != expansion.removed.end() ? name->second.c_str() : "(gone)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Restore")) restored = i;
        ImGui::PopID();
    }
    std::optional<std::size_t> restoredRunner;
    for (std::size_t i = 0; i < overrides.removedRunners.size(); ++i) {
        const auto& entry = overrides.removedRunners[i];
        auto where = prefix;
        where.insert(where.end(), entry.instance.begin(), entry.instance.end());
        const auto name = expansion.removedRunners.find({where, entry.runner});
        ImGui::PushID(static_cast<int>(overrides.removed.size() + i));
        ImGui::TextColored(overriddenColour, "Removed runner %s",
                           name != expansion.removedRunners.end() ? name->second.c_str() : "(gone)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Restore")) restoredRunner = i;
        ImGui::PopID();
    }
    if (!restored && !restoredRunner) return;
    checkpoint();
    if (restored) overrides.removed.erase(overrides.removed.begin() + static_cast<std::ptrdiff_t>(*restored));
    if (restoredRunner)
        overrides.removedRunners.erase(overrides.removedRunners.begin() + static_cast<std::ptrdiff_t>(*restoredRunner));
}

void Editor::runnerInspector(const Runner& shown) {
    const auto track = [this] {
        if (ImGui::IsItemActivated()) editStart_ = doc_;
        if (ImGui::IsItemDeactivatedAfterEdit()) commit(std::move(editStart_));
    };
    auto target = runnerTarget();
    ImGui::TextDisabled(target.addedTo  ? "Added by an instance: kept in its overrides"
                        : target.runner ? "Runner"
                                        : "Runner of an instance: field edits are overrides");
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
                ImGui::Checkbox("Script publishes System", &target.runner->behaviours[k].system);
                track();
            }
            const auto& cached = fieldsOf(behaviour.script, behaviour.system ? "System" : "Behaviour");
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
                                if (selected_.instance < 0) label = instanceLabel(i) + " / " + label;
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

const Editor::CachedFields& Editor::fieldsOf(const std::string& script, const std::string& symbol) {
    auto& entry = fieldCache_[script + "#" + symbol];
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
        entry.fields = ScriptRuntime::fields(project_->root, script, symbol);
        entry.error.clear();
    } catch (const std::exception& failure) {
        entry.fields.clear();
        entry.error = failure.what();
    }
    return entry;
}
}
