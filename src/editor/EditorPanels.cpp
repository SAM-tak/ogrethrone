// The Scene, Systems and Inspector panels and the drill-down breadcrumb of the Editor
// (TDDja/05 6, 06 6).
#include "editor/Editor.h"
#include "editor/OgreConversions.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <OgreMatrix3.h>
#include <OgreQuaternion.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
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

/// ImGui::DragFloat3 for X, Y and Z, each field marked on its left by its axis colour (red,
/// green, blue, as the gizmo draws them).
bool axisDrag3(const char* label, float* values, float speed, float low, float high, const char* format) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x, width = ImGui::CalcItemWidth();
    const bool edited = ImGui::DragFloat3(label, values, speed, low, high, format, ImGuiSliderFlags_AlwaysClamp);
    // Laid out as ImGui::PushMultiItemsWidths() does: equal fields, the last taking the rest.
    const float field = (std::max)(1.0f, std::floor((width - spacing * 2) / 3)), height = ImGui::GetFrameHeight();
    static const ImU32 colours[3] = {IM_COL32(219, 62, 62, 255), IM_COL32(92, 186, 64, 255), IM_COL32(64, 120, 235, 255)};
    auto* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < 3; ++i) {
        const float left = at.x + float(i) * (field + spacing);
        draw->AddRectFilled({left, at.y}, {left + 3, at.y + height}, colours[i], ImGui::GetStyle().FrameRounding,
                            ImDrawFlags_RoundCornersLeft);
    }
    return edited;
}

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
        return nameOf(shown(contexts_.back().path)) + kind;
    }
    const auto* placed = doc_.world.level.findScene(view_.instances[instance].id);
    if (!placed) return "? [Scene]";
    auto label = nameOf(shown(placed->scene)) + " [Scene]";
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
        const auto kind = contexts_[i].kind;
        const auto label = i == 0 ? (levelPath_[0] ? nameOf(levelPath_) : std::string("untitled")) + " [Level]"
                           : kind == Context::Kind::Model ? std::filesystem::u8path(shown(contexts_[i].path)).filename().u8string() + " [Model]"
                                                          : nameOf(shown(contexts_[i].path)) + (kind == Context::Kind::Scene ? " [Scene]" : " [Prefab]");
        if (ImGui::SmallButton(label.c_str()) && i + 1 < contexts_.size() && !runtime_)
            navigate_.emplace(contexts_.begin(), contexts_.begin() + static_cast<std::ptrdiff_t>(i + 1));
        ImGui::PopID();
    }
}

void Editor::scenePanel() {
    // Named after what it shows: the level's scenes, or the hierarchy of a scene, prefab or model.
    if (contexts_.back().kind == Context::Kind::Model) {
        if (ImGui::Begin("Model###Hierarchy", nullptr, ImGuiWindowFlags_HorizontalScrollbar)) modelTree();
        ImGui::End();
        return;
    }
    if (ImGui::Begin(content() ? "Scene###Hierarchy" : "Level###Hierarchy", nullptr, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::BeginDisabled(runtime_ != nullptr);
        // Add Scene and Add Instance place the file selected in the Browser.
        const auto selected = [&](const char* suffix) {
            const auto& path = browserSelection_;
            return path.size() > std::strlen(suffix) && path.compare(path.size() - std::strlen(suffix), std::strlen(suffix), suffix) == 0;
        };
        if (!content()) {
            ImGui::BeginDisabled(!selected(".scene.lton"));
            if (ImGui::Button("Add Scene")) addScene(assets_.bind(AssetRef(browserSelection_)));
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Places the scene selected in the Browser");
            ImGui::SameLine();
        } else if (content()->variant()) {
            ImGui::TextColored(variantColour, "Variant of %s", shown(content()->base).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Open Base")) enter({Context::Kind::Prefab, content()->base});
            if (!view_.instances.empty()) overridesList(content()->overrides, view_.instances[0].expansion, {}, false);
        }
        // Into an instance (at the level, a selected scene instance or a node in one)
        // these add overrides; see insert().
        if (ImGui::Button("Add Node")) addNode();
        ImGui::SameLine();
        ImGui::BeginDisabled(!selected(".prefab.lton"));
        if (ImGui::Button("Add Instance")) addInstance(assets_.bind(AssetRef(browserSelection_)));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Places the prefab selected in the Browser (or drag it here)");
        ImGui::EndDisabled();
        ImGui::Separator();
        // The level itself heads its scenes; selecting it shows its properties.
        if (!content()) {
            const auto name = std::filesystem::u8path(levelPath_).filename().u8string();
            auto flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (selected_.kind == Selection::Kind::Level) flags |= ImGuiTreeNodeFlags_Selected;
            ImGui::TreeNodeEx("##level", flags, "%s [Level]", name.c_str());
            if (ImGui::IsItemClicked()) selected_ = {Selection::Kind::Level};
        }
        if (view_.instances.empty()) ImGui::TextDisabled("No scenes. Add one above.");
        std::optional<std::uint32_t> removed;
        for (std::size_t i = 0; i < view_.instances.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const auto label = instanceLabel(i);
            // At the level a scene instance is a row of its own, selected for its transform.
            auto flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
            // Inside a scene its row stands for the scene file, whose Inspector holds its lighting.
            const bool sceneFile = contexts_.back().kind == Context::Kind::Scene;
            if ((!content() && selected_.kind == Selection::Kind::Scene && selected_.instance == static_cast<int>(i)) ||
                (sceneFile && selected_.kind == Selection::Kind::Level))
                flags |= ImGuiTreeNodeFlags_Selected;
            const bool open = ImGui::TreeNodeEx("##root", flags, "%s", label.c_str());
            if (!content() && ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                selected_ = {Selection::Kind::Scene, static_cast<int>(i)};
            if (sceneFile && ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selected_ = {Selection::Kind::Level};
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
        if (ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->InnerRect, ImGui::GetID("assetDrop"))) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("THRONE_ASSET")) place(static_cast<const char*>(payload->Data));
            ImGui::EndDragDropTarget();
        }
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
        // Double-clicking an instance node drills into its prefab, and a node with a model into the model.
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !runtime_) {
            if (!expanded.node.prefab.empty()) enter({Context::Kind::Prefab, expanded.node.prefab});
            else if (expanded.node.model && !expanded.node.model->path.empty()) enterModel(*expanded.node.model);
        }
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
                               shown(expanded.node.prefab).c_str());
        }
        if (open) {
            nodeTree(instance, k);
            ImGui::TreePop();
        }
    }
}

const Editor::RenderObject* Editor::modelShown() {
    if (contexts_.back().kind != Context::Kind::Model) return nullptr;
    for (const auto& render : renderObjects_)
        if (render.placedModel.root) return &render;
    return nullptr;
}

const ModelLibrary::Contents* Editor::modelContents() {
    const auto* render = modelShown();
    return render ? models_->contents(render->modelKey) : nullptr;
}

void Editor::modelTree() {
    ImGui::TextUnformatted(shown(contexts_.back().path).c_str());
    ImGui::Separator();
    const auto* contents = modelContents();
    if (!contents) {
        const bool reading = std::any_of(renderObjects_.begin(), renderObjects_.end(),
                                         [&](const RenderObject& render) { return modelsReading_.count(render.modelKey) != 0; });
        ImGui::TextDisabled(reading ? "Reading..." : "Nothing to show; see the Console.");
        return;
    }
    const auto& nodes = contents->outline.nodes;
    // Each node's subtree follows it, so siblings are found by skipping subtrees.
    const std::function<void(std::size_t, std::size_t)> rows = [&](std::size_t from, std::size_t to) {
        for (auto k = from; k < to; k = nodes[k].end) {
            const auto& node = nodes[k];
            auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (node.end == k + 1) flags |= ImGuiTreeNodeFlags_Leaf;
            const bool chosen = selected_.kind == Selection::Kind::ModelNode && selected_.item == k;
            if (chosen) flags |= ImGuiTreeNodeFlags_Selected;
            // A node picked elsewhere is shown: the nodes above it open, and it scrolls into view.
            const bool above = selected_.kind == Selection::Kind::ModelNode && selected_.item > k && selected_.item < node.end;
            if (modelReveal_ && above) ImGui::SetNextItemOpen(true);
            const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(k)), flags, "%s", node.name.c_str());
            if (modelReveal_ && chosen) ImGui::SetScrollHereY(.5f);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selected_ = {Selection::Kind::ModelNode, 0, {}, 0, {}, k};
            // A mesh names the materials it uses; they are listed under Materials.
            std::string note = node.joint ? "[Joint]" : "";
            for (const auto m : node.materials) note += (note.empty() ? "" : ", ") + contents->materials[m].name;
            if (!note.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", note.c_str());
            }
            if (open) {
                rows(k + 1, node.end);
                ImGui::TreePop();
            }
        }
    };
    ImGui::PushID("nodes");
    rows(0, nodes.size());
    ImGui::PopID();
    modelReveal_ = false;
    if (ImGui::TreeNodeEx("##materials", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth, "Materials (%zu)",
                          contents->materials.size())) {
        for (std::size_t m = 0; m < contents->materials.size(); ++m) {
            auto flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (selected_.kind == Selection::Kind::ModelMaterial && selected_.item == m) flags |= ImGuiTreeNodeFlags_Selected;
            ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(m)), flags, "%s", contents->materials[m].name.c_str());
            if (ImGui::IsItemClicked()) selected_ = {Selection::Kind::ModelMaterial, 0, {}, 0, {}, m};
        }
        ImGui::TreePop();
    }
}

void Editor::modelInspector() {
    const auto* contents = modelContents();
    if (!contents) return;
    const auto& nodes = contents->outline.nodes;
    const auto& materials = contents->materials;
    // A row selecting another node or material of the model.
    const auto select = [&](Selection::Kind kind, std::size_t item, const std::string& label) {
        ImGui::PushID(kind == Selection::Kind::ModelNode ? "node" : "material");
        ImGui::PushID(static_cast<int>(item));
        if (ImGui::Selectable(label.c_str())) selected_ = {kind, 0, {}, 0, {}, item}, modelReveal_ = true;
        ImGui::PopID();
        ImGui::PopID();
    };
    if (selected_.kind == Selection::Kind::ModelNode && selected_.item < nodes.size()) {
        const auto& node = nodes[selected_.item];
        ImGui::TextUnformatted(node.name.c_str());
        ImGui::TextDisabled("%s as read; not editable", node.joint ? "Joint" : node.triangles ? "Mesh" : "Node");
        if (node.parent >= 0) {
            ImGui::TextUnformatted("Parent:");
            ImGui::SameLine();
            select(Selection::Kind::ModelNode, std::size_t(node.parent), nodes[std::size_t(node.parent)].name);
        }
        // Rotation as a Transform's: degrees about X, then Y, then Z.
        Ogre::Matrix3 axes;
        Ogre::Quaternion(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]).ToRotationMatrix(axes);
        Ogre::Radian z, y, x;
        axes.ToEulerAnglesZYX(z, y, x);
        ImGui::SeparatorText(node.joint ? "In the model (file pose)" : "In the model");
        ImGui::Text("Position  %.4f, %.4f, %.4f", node.position.x, node.position.y, node.position.z);
        ImGui::Text("Rotation  %.2f, %.2f, %.2f", float(x.valueDegrees()), float(y.valueDegrees()), float(z.valueDegrees()));
        ImGui::Text("Scale     %.4f, %.4f, %.4f", node.scale.x, node.scale.y, node.scale.z);
        if (node.lower.x <= node.upper.x) {
            const auto size = node.upper - node.lower;
            ImGui::Text("Bounds    %.3f x %.3f x %.3f m", size.x, size.y, size.z);
        }
        if (node.triangles) {
            ImGui::SeparatorText("Mesh");
            ImGui::Text("%zu vertices, %zu triangles", node.vertices, node.triangles);
            for (const auto m : node.materials) select(Selection::Kind::ModelMaterial, m, materials[m].name);
        }
    } else if (selected_.kind == Selection::Kind::ModelMaterial && selected_.item < materials.size()) {
        const auto& material = materials[selected_.item];
        ImGui::TextUnformatted(material.name.c_str());
        ImGui::TextDisabled("Material as read; not editable");
        const char* surface = material.renderQueue == 99 ? "Absorbing volume"
                              : material.blended         ? "Refractive"
                              : material.alphaHashed     ? "Alpha Hash"
                              : material.alphaTested     ? "Alpha Test"
                                                         : "Opaque";
        ImGui::Text("Surface   %s", surface);
        if (material.blended) ImGui::Text("Opacity   %.3f", material.opacity);
        if (material.diffuse.empty()) ImGui::Text("Colour    %.3f, %.3f, %.3f", material.colour.x, material.colour.y, material.colour.z);
        if (!material.emissive.empty()) ImGui::Text("Emission  %.3f", material.emission);
        ImGui::SeparatorText("Textures");
        // Shown relative to the project when they lie in it.
        const auto texture = [&](const char* label, const std::filesystem::path& path) {
            auto shown = path.lexically_relative(project_->resolve(""));
            if (shown.empty() || *shown.begin() == "..") shown = path;
            ImGui::Text("%-9s %s", label, path.empty() ? "none" : shown.generic_u8string().c_str());
        };
        texture("Diffuse", material.diffuse);
        texture("Normal", material.normal);
        texture("ORM", material.orm);
        texture("Emissive", material.emissive);
        ImGui::SeparatorText("Used by");
        for (std::size_t k = 0; k < nodes.size(); ++k)
            if (std::find(nodes[k].materials.begin(), nodes[k].materials.end(), selected_.item) != nodes[k].materials.end())
                select(Selection::Kind::ModelNode, k, nodes[k].name);
    }
}

void Editor::systemsPanel() {
    if (ImGui::Begin("Systems", nullptr, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::BeginDisabled(runtime_ != nullptr);
        auto& level = doc_.world.level;
        bool serial = level.serialExecution;
        if (ImGui::Checkbox("No frame delay (serial)", &serial)) checkpoint(), level.serialExecution = serial;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("On: rendering shows this frame's simulation, after it.\n"
                              "Off: rendering shows the previous frame while simulation runs on a worker thread.");
        static char newStage[256] = "CustomSystem";
        ImGui::InputText("System name", newStage, sizeof newStage);
        if (ImGui::Button("Add System Stage") && newStage[0] && !isEngineStage(newStage)) {
            const auto stages = ScriptRuntime::schedule(doc_.world, view_);
            if (std::find(stages.begin(), stages.end(), newStage) == stages.end()) {
                checkpoint();
                level.stages.insert(std::find(level.stages.begin(), level.stages.end(), "Animation"), newStage);
            }
        }
        if (ImGui::Button("+ Script System")) addSystem();
        ImGui::EndDisabled();
        const auto stages = ScriptRuntime::schedule(doc_.world, view_);
        // Display names can repeat; keep row identity tied to the full instance path.
        const auto rowId = [](const char* kind, int instance, const NodePath& path, std::uint64_t item) {
            auto id = std::string(kind) + "/" + std::to_string(instance);
            for (const auto step : path) id += "/" + std::to_string(step);
            return id + "/" + std::to_string(item);
        };
        const auto direct = [&](const System& system, int instance, const NodePath& path) {
            ImGui::PushID(rowId("system", instance, path, system.id).c_str());
            const auto label = system.name + "###system";
            if (ImGui::Selectable(label.c_str())) selected_ = {Selection::Kind::System, instance, path, system.id};
            for (const auto& behaviour : system.behaviour)
                ImGui::BulletText("%s [Behaviour]", shown(behaviour.script).c_str());
            ImGui::PopID();
        };
        // Native engine stages are leaves; OGRE's Inspector holds the rendering settings.
        const auto engineStage = [&](const std::string& name) {
            if (ImGui::Selectable(name.c_str(), selected_.kind == Selection::Kind::EngineSystem && selected_.engine == name))
                selected_ = {Selection::Kind::EngineSystem, -1, {}, 0, name};
            if (!ImGui::IsItemHovered()) return;
            if (name == "OGRE") ImGui::SetTooltip("Rendering settings");
            if (name == "SteamAudio") ImGui::SetTooltip("Updates source directions, distance attenuation, occlusion and transmission");
            if (name == "SoLoud") ImGui::SetTooltip("Applies playback commands, source settings and audio parameters");
            if (name == "SyncSimulation") ImGui::SetTooltip("Copies the simulation's scene to the render snapshot");
            if (name == "SyncRender") ImGui::SetTooltip("Joins rendering and simulation; the frame ends");
            if (name == renderStages[0]) ImGui::SetTooltip("Runs RenderEvent.prerender on the render snapshot");
            if (name == renderStages[2]) ImGui::SetTooltip("Runs RenderEvent.postrender on the render snapshot");
        };
        const auto stageRow = [&](std::size_t index) {
            const auto& stage = stages[index];
            const int phase=behaviourPhase(stage);
            if (isEngineStage(stage) && phase < 0) return engineStage(stage);
            ImGui::PushID(stage.c_str());
            const bool open = ImGui::TreeNodeEx(stage.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            if (phase < 0) {
                ImGui::BeginDisabled(runtime_ != nullptr);
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
                bool used = false;
                for (const auto& system : level.systems) used |= system.name == stage;
                for (const auto& instance : view_.instances) {
                    for (const auto& system : instance.expansion.systems) used |= system.system.name == stage;
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(used);
                if (ImGui::SmallButton("Remove Stage")) {
                    checkpoint();
                    level.stages = stages;
                    level.stages.erase(level.stages.begin() + static_cast<std::ptrdiff_t>(index));
                }
                ImGui::EndDisabled();
                ImGui::EndDisabled();
            }
            if (open) {
                // Script Systems own their stage; BehaviourSystems query node attachments below.
                for (std::size_t i = 0; i < view_.instances.size(); ++i)
                    for (const auto& system : view_.instances[i].expansion.systems)
                        if (system.system.name == stage) direct(system.system, static_cast<int>(i), system.instance);
                for (const auto& system : level.systems)
                    if (system.name == stage) direct(system, -1, {});
                if(phase>=0) ImGui::TextDisabled("Query: enabled Behaviour.%s | order_by: effective priority",behaviourMethods[phase]);
                std::vector<BehaviourMatch> matches;
                if(runtime_) matches=runtime_->behaviourMatches(stage);
                else if(phase>=0) {
                    for(std::size_t i=0;i<view_.instances.size();++i)
                        for(const auto n:view_.instances[i].expansion.hierarchy()) {
                            const auto& behaviour=view_.instances[i].expansion.nodes[n].node.behaviour;
                            for(std::size_t k=0;k<behaviour.size();++k) {
                                const auto& b=behaviour[k];
                                if(!b.enabled) continue;
                                const auto& cached=fieldsOf(b.script,"Behaviour");
                                const bool startOnly=cached.behaviour.start &&
                                    std::none_of(cached.behaviour.methods.begin(),cached.behaviour.methods.end(),[](bool present) { return present; });
                                if(!cached.error.empty() || (!cached.behaviour.methods[phase] && !(phase==0 && startOnly))) continue;
                                matches.push_back({static_cast<int>(i),n,k,effectivePriority(b,cached.behaviour.defaults,phase),shown(b.script),false});
                            }
                        }
                    std::stable_sort(matches.begin(),matches.end(),[](const auto& a,const auto& b) { return a.priority<b.priority; });
                }
                for(const auto& match:matches) {
                    const auto& node=view_.instances[match.scene].expansion.nodes[match.node];
                    ImGui::PushID(rowId("node",match.scene,node.path,match.attachment).c_str());
                    const auto text=instanceLabel(match.scene)+" / "+node.node.name+" / "+match.script+
                        (match.waiting?" [resume]":"")+"###attachment";
                    ImGui::Text("%.9g",match.priority); ImGui::SameLine();
                    if(ImGui::Selectable(text.c_str())) selected_={Selection::Kind::Node,match.scene,node.path};
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        };
        const auto group = [&](const char* name, const char* tip, const std::function<void()>& rows) {
            const bool open = ImGui::TreeNodeEx(name, ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            if (!open) return;
            rows();
            ImGui::TreePop();
        };
        const auto loopEnd = static_cast<std::size_t>(std::find(stages.begin(), stages.end(), behaviourSystems[2]) - stages.begin());
        const auto simulation = [&] {
            stageRow(0);
            group("UpdateLoop", "Runs once per elapsed 1/60 s", [&] { for (std::size_t i = 1; i < loopEnd; ++i) stageRow(i); });
            for (std::size_t i = loopEnd; i < stages.size(); ++i) stageRow(i);
        };
        const auto rendering = [&] { for (const char* stage : renderStages) engineStage(stage); };
        if (level.serialExecution) {
            simulation();
            engineStage("SyncSimulation");
            rendering();
        } else {
            engineStage("SyncSimulation");
            group("Parallel", "Rendering draws the previous frame while simulation computes this one", [&] {
                group("Rendering", "Main thread", rendering);
                group("Simulation", "Worker thread", simulation);
            });
        }
        engineStage("SyncRender");
        const auto invalid = [&](const System& system) {
            return std::find(stages.begin(), stages.end(), system.name) == stages.end();
        };
        for (std::size_t i = 0; i < view_.instances.size(); ++i)
            for (const auto& system : view_.instances[i].expansion.systems)
                if (invalid(system.system)) direct(system.system, static_cast<int>(i), system.instance);
        for (const auto& system : level.systems)
            if (invalid(system)) direct(system, -1, {});
    }
    ImGui::End();
}

void Editor::inspector() {
    if (ImGui::Begin("Inspector")) {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float right = start.x + ImGui::GetContentRegionAvail().x;
        // Widgets end where the label column starts; text wraps at the panel's width.
        ImGui::PushItemWidth(-preferences_.inspectorLabelWidth);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::BeginDisabled(runtime_ != nullptr);
        // Read-only pages have no labelled widgets, so no label column to size.
        bool labelled = true;
        if (selected_.kind==Selection::Kind::EngineSystem) {
            ImGui::TextUnformatted(selected_.engine.c_str());
            if(selected_.engine=="OGRE") rendererInspector();
            else labelled = false;
        }
        else if (selected_.kind==Selection::Kind::ModelNode || selected_.kind==Selection::Kind::ModelMaterial) modelInspector(), labelled = false;
        else if (const auto* node = selectedNode()) nodeInspector(*node);
        else if (const auto* system = selectedSystem()) systemInspector(*system);
        else if (auto* placed = selectedScene()) sceneInspector(*placed);
        else if (selected_.kind==Selection::Kind::Level && !content()) levelInspector();
        else if (selected_.kind==Selection::Kind::Level && contexts_.back().kind==Context::Kind::Scene) {
            ImGui::TextUnformatted(shown(contexts_.back().path).c_str());
            lightingInspector(contexts_.back().path);
        }
        else ImGui::TextDisabled("Nothing selected"), labelled = false;
        ImGui::EndDisabled();
        ImGui::PopTextWrapPos();
        ImGui::PopItemWidth();
        if (labelled) labelColumnHandle(start, right);
    }
    ImGui::End();
}

void Editor::labelColumnHandle(ImVec2 start, float right) {
    auto& width = preferences_.inspectorLabelWidth;
    const float size = ImGui::GetTextLineHeight(), gap = ImGui::GetStyle().ItemInnerSpacing.x;
    // Both columns keep some room however narrow the panel is.
    width = std::clamp(width, size, (std::max)(size, right - start.x - 2 * size));
    const float edge = right - width, bottom = ImGui::GetCursorScreenPos().y;
    if (bottom <= start.y) return;
    // The gap between the widgets and their labels, from the top of the Inspector to its end,
    // drags the edge. Widgets drawn over it (headers) keep the mouse.
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos({edge - 3, start.y});
    ImGui::InvisibleButton("##labelColumn", {gap + 6, bottom - start.y});
    const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    if (active) width -= ImGui::GetIO().MouseDelta.x;
    if (active || hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        const auto colour = ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive : ImGuiCol_SeparatorHovered);
        ImGui::GetWindowDrawList()->AddLine({edge + gap * .5f, start.y}, {edge + gap * .5f, bottom}, colour);
    }
    if (ImGui::IsItemDeactivated()) {
        try { preferences_.save(preferencesFile_); }
        catch (const std::exception& error) { log(std::string("Could not save editor preferences: ") + error.what()); }
    }
    ImGui::SetCursorScreenPos(cursor);
}

void Editor::selectRenderer() { selected_={Selection::Kind::EngineSystem,-1,{},0,"OGRE"}; }

void Editor::rendererInspector() {
    ImGui::SeparatorText("Game rendering (project)");
    bool vsync=doc_.gameRendering.vsync;
    if(ImGui::Checkbox("VSync",&vsync)) { checkpoint(); doc_.gameRendering.vsync=vsync; }
    ImGui::TextWrapped("Editor Play uses the VSync control in viewport statistics.");
    ImGui::TextWrapped("A level's rendering is set in its Inspector (the Level panel's first row); a scene's "
                       "environment and global illumination in the scene's.");
}

void Editor::lightingInspector(const AssetRef& path) {
    const auto found=doc_.world.scenes.find(path);
    if(found==doc_.world.scenes.end()) return;
    auto& lighting=found->second.lighting;
    // The view shows one scene's environment and GI: the scene edited, or the level's first.
    const bool visible=path==lightingScenePath() && giInstance()>=0;
    const auto track=[&] {
        if(ImGui::IsItemActivated()) editStart_=doc_;
        if(ImGui::IsItemDeactivatedAfterEdit()) commit(std::move(editStart_));
    };
    ImGui::SeparatorText("Environment");
    if(!visible) ImGui::TextDisabled("Not shown: the view uses the level's first scene's environment.");
    bool defaultSun=lighting.defaultSun;
    if(ImGui::Checkbox("Default Sun",&defaultSun)) { checkpoint(); lighting.defaultSun=defaultSun; }
    ImGui::SameLine();
    bool sky=lighting.sky;
    if(ImGui::Checkbox("Sky",&sky)) {
        checkpoint(); lighting.sky=sky;
        if(visible) setSkyboxEnabled(sky);
    }
    auto ambientUpper=lighting.ambientUpper,ambientLower=lighting.ambientLower;
    const bool ambientUpperEdited=ImGui::ColorEdit3("Ambient Upper",ambientUpper.data()); track();
    const bool ambientLowerEdited=ImGui::ColorEdit3("Ambient Lower",ambientLower.data()); track();
    if(ambientUpperEdited) lighting.ambientUpper=ambientUpper;
    if(ambientLowerEdited) lighting.ambientLower=ambientLower;
    char environment[256]; std::snprintf(environment,sizeof environment,"%s",shown(lighting.environment).c_str());
    if(ImGui::InputText("Environment (HDR)",environment,sizeof environment,ImGuiInputTextFlags_EnterReturnsTrue)) {
        checkpoint(); lighting.environment=assets_.bind(std::string(environment));
    }
    float scale=lighting.environmentScale;
    const bool scaleEdited=ImGui::DragFloat("Environment Scale",&scale,.01f,0,100); track();
    if(scaleEdited) lighting.environmentScale=(std::max)(scale,0.0f);
    float temperature=lighting.environmentTemperature;
    const bool temperatureEdited=ImGui::DragFloat("Environment Temperature (K)",&temperature,10,0,40000); track();
    if(temperatureEdited) lighting.environmentTemperature=(std::max)(temperature,0.0f);
    bool toneMapping=lighting.toneMapping;
    if(ImGui::Checkbox("Filmic Tone Mapping",&toneMapping)) { checkpoint(); lighting.toneMapping=toneMapping; }
    float exposure=lighting.exposure;
    const bool exposureEdited=ImGui::DragFloat("Exposure (EV)",&exposure,.05f,-16,16); track();
    if(exposureEdited) lighting.exposure=exposure;

    ImGui::SeparatorText("Global illumination (baked)");
    auto& gi=lighting.gi;
    bool enabled=gi.enabled;
    if(ImGui::Checkbox("Enabled",&enabled)) { checkpoint(); gi.enabled=enabled; }
    ImGui::BeginDisabled(!gi.enabled || gi_->baking());
    const char* volumes[]={"Global (whole scene)","Local (box)"};
    int volume=gi.localProbeVolume?1:0;
    if(ImGui::Combo("Probe Volume",&volume,volumes,2)) { checkpoint(); gi.localProbeVolume=volume==1; }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("What the probes cover: the bounds of everything drawn, or the box below.");
    // The box is only used by the Local volume.
    ImGui::BeginDisabled(!gi.localProbeVolume);
    auto lower=gi.lower,upper=gi.upper;
    const bool lowerEdited=ImGui::DragFloat3("Box Lower",lower.data(),.1f); track();
    const bool upperEdited=ImGui::DragFloat3("Box Upper",upper.data(),.1f); track();
    if(lowerEdited || upperEdited) {
        for(int i=0;i<3;++i) upper.data()[i]=(std::max)(upper.data()[i],lower.data()[i]+.1f);
        gi.lower=lower; gi.upper=upper;
    }
    if(ImGui::Button("Fit to Scene")) {
        // Every drawn node's bounds; the floor is left alone (it sits a little above the
        // ground, or probes under it see the sky through the ground's back faces).
        Vec3 low,high;
        if(sceneBounds(low,high,giInstance())) { checkpoint(); gi.lower.x=low.x; gi.lower.z=low.z; gi.upper.x=high.x; gi.upper.y=(std::max)(high.y,gi.lower.y+.1f); gi.upper.z=high.z; }
    }
    ImGui::EndDisabled();
    auto spacing=gi.probeSpacing;
    // Finer than the surface points' cells (ModelLibrary::surfaceCell) places nothing better.
    const bool spacingEdited=ImGui::DragFloat3("Finest Spacing (m)",spacing.data(),.05f,ModelLibrary::surfaceCell,50,"%.2f"); track();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("The finest distance between probes along X, Y and Z.");
    if(spacingEdited)
        for(int i=0;i<3;++i) gi.probeSpacing.data()[i]=std::clamp(spacing.data()[i],ModelLibrary::surfaceCell,50.0f);
    auto offset=gi.probeOffset;
    const bool offsetEdited=ImGui::DragFloat3("Probe Offset (m)",offset.data(),.05f,-1000,1000,"%.2f"); track();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Probes lie at this offset plus whole multiples of the spacing\n(Y: the height of the lowest layers, such as the floor).");
    if(offsetEdited) gi.probeOffset=offset;
    float minVolume=gi.minObjectVolume;
    const bool minVolumeEdited=ImGui::DragFloat("Min Object Volume (m3)",&minVolume,.01f,0,1000,"%.2f"); track();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Objects whose bounds hold less than this place no probes around them.");
    if(minVolumeEdited) gi.minObjectVolume=(std::max)(minVolume,0.0f);
    bool fill=gi.fillEmptySpaces;
    if(ImGui::Checkbox("Fill Empty Spaces",&fill)) { checkpoint(); gi.fillEmptySpaces=fill; }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Also covers empty space far from any surface, with the coarsest cells.");
    // The probes placed and baked are those of the scene whose GI is shown.
    if(visible) updateProbeLayout();
    unsigned perLevel[ProbeOctree::levels]={};
    for(const auto& cell:octree_.cells) ++perLevel[cell.level];
    std::size_t rendered=0;
    for(const auto& probe:octree_.probes) rendered+=probe.from<0;
    ImGui::TextDisabled("%zu cells (L0 %u, L1 %u, L2 %u, L3 %u, L4 %u, L5 %u)%s",octree_.cells.size(),perLevel[0],perLevel[1],perLevel[2],perLevel[3],perLevel[4],perLevel[5],placing_?" (placing...)":"");
    ImGui::TextDisabled("%zu probes: %zu rendered, %zu interpolated",octree_.probes.size(),rendered,octree_.probes.size()-rendered);
    // Reflection probes stand on a grid over the probes' volume too.
    const auto size=layoutUpper_-layoutLower_;
    float reflection[3]={size.x/float(gi.reflectionProbes[0]),size.y/float(gi.reflectionProbes[1]),size.z/float(gi.reflectionProbes[2])};
    if(ImGui::DragFloat3("Reflection Spacing (m)",reflection,.05f,.5f,200,"%.1f")) {
        checkpoint();
        for(int i=0;i<3;++i)
            gi.reflectionProbes[i]=std::clamp(unsigned(std::lround((&size.x)[i]/(std::max)(reflection[i],.5f))),1u,16u);
    }
    const unsigned reflectionCount=gi.reflectionProbes[0]*gi.reflectionProbes[1]*gi.reflectionProbes[2];
    // Per reflection probe: a 256x256 RGBA16F cubemap with its mipmaps.
    const double megabytes=reflectionCount*6.0*256*256*8*4/3/(1024.0*1024);
    ImGui::TextDisabled("%u x %u x %u reflection probes, about %.0f MB",
                        gi.reflectionProbes[0],gi.reflectionProbes[1],gi.reflectionProbes[2],megabytes);
    bool fastShadows=gi.fastBakeShadows;
    if(ImGui::Checkbox("Fast Bake Shadows",&fastShadows)) { checkpoint(); gi.fastBakeShadows=fastShadows; }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Diffuse probes bake with one 2048x2048 map for the sun's shadow\ninstead of the viewport's three cascades: faster.");
    int bounces=int(gi.bounces);
    if(ImGui::SliderInt("Bounces",&bounces,1,8) && unsigned(bounces)!=gi.bounces) { checkpoint(); gi.bounces=unsigned(bounces); }
    // A bake renders a cubemap from every probe once per bounce: the reflection probes and the
    // diffuse probes that are not interpolated (TDDja/12 4).
    ImGui::TextDisabled("Cubemap renders per bake: %zu (x%u bounces: %u reflection + %zu diffuse)",gi.bounces*(reflectionCount+rendered),gi.bounces,reflectionCount,rendered);
    ImGui::BeginDisabled(!visible);
    if(ImGui::Button("Bake")) requestGlobalIlluminationBake();
    ImGui::EndDisabled();
    if(!visible) ImGui::TextDisabled("Only the GI of the level's first scene is shown and baked for now.");
    ImGui::EndDisabled();
    if(!visible) return;
    bool probes=gi_->debugVisible();
    if(ImGui::Checkbox("Show Probes",&probes)) gi_->setDebugVisible(probes);
    ImGui::SameLine();
    if(ImGui::Checkbox("Show Surface Points",&showSurface_)) giBoxesLayout_=~0u;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("With Show Probes: the surface points that place probes (red),\nand those left out by Min Object Volume (grey).");
    if(!gi_->active()) ImGui::TextDisabled("No bake in use (%s)",giFolder().filename().u8string().c_str());
    else if(const auto seconds=gi_->bakeSeconds(); seconds>0) {
        const auto whole=long(std::lround(seconds));
        ImGui::TextDisabled(whole>=60 ? "Using the bake in %s (baked in %ld min %02ld s)" : "Using the bake in %s (baked in %ld s)",
                            giFolder().filename().u8string().c_str(),whole>=60 ? whole/60 : whole,whole%60);
    } else ImGui::TextDisabled("Using the bake in %s",giFolder().filename().u8string().c_str());
    if(gi_->active() && gi_->settings()!=gi) ImGui::TextColored(ImVec4(1,.75f,.3f,1),"Settings changed since the bake: bake again to apply them.");
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
    // Overrides to write into an instance's prefab, or to take out of target.overrides,
    // once nothing here points into them.
    std::optional<std::pair<SceneNode*, Overrides>> applying;
    std::optional<Overrides> reverting;
    // Reverts or applies one overridden property, given as a member of NodeOverride.
    const auto revertOrApply = [&](auto member, const char* revert, const char* apply) {
        if (ImGui::SmallButton(revert)) {
            checkpoint();
            (layer->*member).reset();
        }
        if (!target.holder) return;
        ImGui::SameLine();
        if (ImGui::SmallButton(apply)) {
            Overrides piece;
            piece.node(target.relative).*member = layer->*member;
            applying.emplace(target.holder, std::move(piece));
        }
    };
    // Draws one property, coloured when overridden, with Revert and Apply next to it.
    const auto property = [&](unsigned bit, auto member, auto&& widget) {
        const bool overridden = layer && (layer->*member).has_value();
        if (overridden) ImGui::PushStyleColor(ImGuiCol_Text, overriddenColour);
        if (widget()) changed |= bit;
        if (overridden) ImGui::PopStyleColor();
        track();
        if (overridden) {
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(bit));
            revertOrApply(member, "Revert", "Apply");
            ImGui::PopID();
        }
    };
    constexpr auto clamp = ImGuiSliderFlags_AlwaysClamp;
    char name[256];
    fill(name, value.name);
    property(1, &NodeOverride::name,
             [&] { return ImGui::InputText("Name", name, sizeof name) && name[0] && (value.name = name, true); });
    // Components, one section each. A plain node the edited file holds can add and
    // remove them; elsewhere only their properties change (as overrides).
    auto* own = target.node && target.node->prefab.empty() ? target.node : nullptr;
    bool keepTransform = true, keepBox = true;
    constexpr auto open = ImGuiTreeNodeFlags_DefaultOpen;
    if (auto& transform = value.transform;
        transform && ImGui::CollapsingHeader("Transform", own ? &keepTransform : nullptr, open)) {
        property(2, &NodeOverride::position,
                 [&] { return axisDrag3("Position", transform->position.data(), 0.05f, -1.0e6f, 1.0e6f, "%.3f"); });
        property(4, &NodeOverride::rotation,
                 [&] { return axisDrag3("Rotation", transform->rotation.data(), 0.5f, -1.0e6f, 1.0e6f, "%.1f"); });
        property(8, &NodeOverride::scale,
                 [&] { return axisDrag3("Scale", transform->scale.data(), 0.01f, 0.001f, 1.0e6f, "%.3f"); });
    }
    if (auto& box = value.box; box && ImGui::CollapsingHeader("Box Primitive", own ? &keepBox : nullptr, open))
        property(16, &NodeOverride::colour, [&] { return ImGui::ColorEdit3("Colour", box->colour.data()); });
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
    const auto componentSection = [&](const char* label, bool removable, auto member, auto fields, auto remove) {
        ImGui::PushID(label);
        bool keep=true;
        if (ImGui::CollapsingHeader(label,removable?&keep:nullptr,open)) fields();
        if (!keep) { checkpoint(); remove(); }
        if (layer && (layer->*member).has_value()) revertOrApply(member, "Revert Component", "Apply Component");
        ImGui::PopID();
    };
    // Components inherited from a prefab permit value overrides, but not removal.
    const auto component = [&](const char* label, auto& shown, auto member, auto overrideMember, auto fields) {
        if (!shown) return;
        componentSection(label,own!=nullptr,overrideMember,[&] {
            if (!fields(*shown)) return;
            if (own) own->*member=shown;
            else if(target.overrides) target.overrides->node(target.relative).*overrideMember=shown;
        },[&] { (own->*member).reset(); });
    };
    // A text field for a name, or a file reference: it shows the file's path, and a path typed
    // in refers to the asset there (TDDja/15 5.2).
    const auto texturePath=[&](const char* label,auto& value) {
        constexpr bool reference=std::is_same_v<std::decay_t<decltype(value)>,AssetRef>;
        char text[256];
        if constexpr (reference) fill(text,shown(value)); else fill(text,value);
        const bool edited=ImGui::InputText(label,text,sizeof text); track();
        if(edited) {
            if constexpr (reference) value=assets_.bind(std::string(text)); else value=text;
        }
        return edited;
    };
    // Light passing through thin surfaces from behind; black turns it off.
    const auto translucency=[&](Translucency& t) {
        bool edited=ImGui::ColorEdit3("Translucency",t.colour.data(),ImGuiColorEditFlags_HDR|ImGuiColorEditFlags_Float); track();
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Light passing through the surface from behind it (leaves, paper, cloth,\n"
                                                     "wax), in this colour; black turns it off. Shadows still hide it, so it\n"
                                                     "suits thin surfaces.");
        if(t.enabled()) {
            edited|=scalar("Translucency Distortion",t.distortion,0,1);
            edited|=scalar("Translucency Power",t.power,.1f,256);
            edited|=scalar("Translucency Scale",t.scale,0,100);
        }
        return edited;
    };
    const auto subsurface=[&](float& width) {
        bool edited=ImGui::DragFloat("Subsurface Width (m)",&width,.0005f,0,10,width>0?"%.4f":"off",ImGuiSliderFlags_AlwaysClamp); track();
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("How far light spreads under the surface (skin about 0.01): its lighting is\n"
                                                     "blurred on screen by a skin-like profile, red furthest. 0 turns it off.");
        return edited;
    };
    component("Model",value.model,&SceneNode::model,&NodeOverride::model,[&](auto& c) {
        bool edited=texturePath("File",c.path);
        edited|=ImGui::Checkbox("DirectX normal maps",&c.directXNormals); track();
        edited|=ImGui::DragFloat("Emission",&c.emission,.1f,0,100000); track();
        edited|=ImGui::Checkbox("Alpha-Hashed Cut-Outs",&c.alphaHashing); track();
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Cut-out materials (diffuse maps with alpha) keep each pixel with the\n"
                                                     "probability of its alpha instead of testing it against 0.5: soft edges\n"
                                                     "once temporal anti-aliasing averages them.");
        ImGui::TextDisabled("An FBX file (or an OGRE .mesh): its meshes and PBS materials.");
        if(!c.path.empty() && ImGui::SmallButton("Open Model")) enterModel(c);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Shows the model alone, with its nodes and materials as read.");
        // Changes to named materials of the file: absorbing volumes such as liquids.
        for(std::size_t i=0;i<c.materials.size();) {
            ImGui::PushID(int(i));
            auto& m=c.materials[i];
            ImGui::Separator();
            edited|=texturePath("Material",m.name);
            edited|=texturePath("Diffuse Map",m.diffuseMap);
            edited|=vector("Absorption (1/m)",m.absorption);
            edited|=scalar("Thickness (m)",m.thickness,.0001f,100);
            edited|=translucency(m.translucency);
            edited|=subsurface(m.subsurfaceWidth);
            const bool removed=ImGui::SmallButton("Remove Material Change");
            ImGui::PopID();
            if(removed) { c.materials.erase(c.materials.begin()+std::ptrdiff_t(i)); edited=true; }
            else ++i;
        }
        if(ImGui::SmallButton("Add Material Change")) { c.materials.push_back({}); edited=true; }
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
        edited|=texturePath("Reflection Cubemap",c.reflectionMap);
        int alpha=static_cast<int>(c.alpha);
        if(ImGui::Combo("Alpha",&alpha,"Opaque\0Alpha Test\0Alpha Hash\0")) { c.alpha=static_cast<AlphaMode>(alpha); edited=true; }
        track();
        if(c.alpha==AlphaMode::Test) edited|=scalar("Alpha Threshold",c.alphaThreshold,0,1);
        if(c.alpha==AlphaMode::Hash) {
            edited|=scalar("Opacity",c.opacity,0,1);
            ImGui::TextDisabled("Each pixel is kept with the probability of its alpha;\ntemporal anti-aliasing averages it.");
        }
        edited|=translucency(c.translucency);
        edited|=subsurface(c.subsurfaceWidth);
        return edited;
    });
    component("Camera",value.camera,&SceneNode::camera,&NodeOverride::camera,[&](auto& c) {
        bool edited=scalar("Field of View (deg)",c.fieldOfView,1,170);
        edited|=scalar("Near Clip",c.nearClip,.001f,1.0e6f);
        edited|=scalar("Far Clip",c.farClip,.001f,1.0e7f);
        c.farClip=(std::max)(c.farClip,c.nearClip*1.001f);
        edited|=scalar("Focus Distance",c.focusDistance,.01f,1.0e6f);
        edited|=scalar("F-Stop (0: no depth of field)",c.fStop,0,64);
        edited|=antiAliasingCombo(c.antiAliasing);
        // The view and the camera, either way; the camera looks along the node's -Z.
        if(ImGui::Button("Align View to Camera")) alignViewToCamera(std::size_t(selected_.instance),std::size_t(&expanded-view_.instances[std::size_t(selected_.instance)].expansion.nodes.data()),c);
        ImGui::SameLine();
        if(ImGui::Button("Align Camera to View")) alignCameraToView();
        return edited;
    });
    component("Audio Source",value.audioSource,&SceneNode::audioSource,&NodeOverride::audioSource,[&](auto& c) {
        bool edited=texturePath("Clip",c.clip);
        edited|=ImGui::Checkbox("Enabled",&c.enabled); track();
        edited|=ImGui::Checkbox("3D",&c.spatial); track();
        edited|=ImGui::Checkbox("Loop",&c.loop); track();
        edited|=ImGui::Checkbox("Auto Play",&c.autoPlay); track();
        edited|=ImGui::Checkbox("Streaming (2D only)",&c.streaming); track();
        edited|=scalar("Volume",c.volume,0,100);
        if(c.spatial) {
            edited|=scalar("Min Distance (m)",c.minDistance,0,999999);
            edited|=scalar("Max Distance (m)",c.maxDistance,.001f,1.0e6f);
            c.maxDistance=(std::max)(c.maxDistance,c.minDistance+.001f);
            edited|=ImGui::Checkbox("Occlusion",&c.occlusion); track();
            ImGui::BeginDisabled(!c.occlusion);
            edited|=ImGui::Checkbox("Transmission",&c.transmission); track();
            ImGui::EndDisabled();
            edited|=ImGui::Checkbox("Reflections / reverb",&c.reflections); track();
            edited|=ImGui::DragFloat("Reflection volume",&c.reflectionVolume,.01f,0,10,"%.3f",clamp); track();
            ImGui::TextDisabled("Mono, preloaded clips only");
        }
        const auto found=doc_.mixers.find(doc_.world.level.mixer);
        if(found!=doc_.mixers.end()) {
            std::string current="Master";
            for(const auto& bus:found->second.buses) if(bus.id==c.bus) current=bus.name;
            if(ImGui::BeginCombo("Output Bus",current.c_str())) {
                for(const auto& bus:found->second.buses) if(ImGui::Selectable(bus.name.c_str(),bus.id==c.bus)) {
                    checkpoint(); c.bus=bus.id; edited=true;
                }
                ImGui::EndCombo();
            }
        } else {
            int bus=static_cast<int>((std::min)(c.bus,std::uint32_t(INT_MAX)));
            if(ImGui::InputInt("Output Bus ID",&bus)) { c.bus=static_cast<std::uint32_t>((std::max)(1,bus)); edited=true; } track();
        }
        return edited;
    });
    component("Acoustic Geometry",value.acousticGeometry,&SceneNode::acousticGeometry,&NodeOverride::acousticGeometry,[&](auto& c) {
        bool edited=ImGui::Checkbox("Enabled",&c.enabled); track();
        int shape=static_cast<int>(c.shape);
        edited|=ImGui::Combo("Shape",&shape,"Render Geometry\0Box Proxy\0"); track(); c.shape=static_cast<AcousticShape>(shape);
        if(c.shape==AcousticShape::Box) edited|=vector("Box Size (m)",c.boxSize,.001f);
        else ImGui::TextDisabled("Uses this node's primitives and FBX rest geometry");
        edited|=ImGui::DragFloat3("Absorption (low/mid/high)",c.absorption.data(),.01f,0,1,"%.3f",clamp); track();
        edited|=scalar("Scattering",c.scattering,0,1);
        edited|=ImGui::DragFloat3("Transmission (low/mid/high)",c.transmission.data(),.01f,0,1,"%.3f",clamp); track();
        return edited;
    });
    component("Audio Listener",value.audioListener,&SceneNode::audioListener,&NodeOverride::audioListener,[&](auto& c) {
        bool edited=ImGui::Checkbox("Enabled",&c.enabled); track();
        edited|=ImGui::InputInt("Priority",&c.priority); track(); return edited;
    });
    component("Animator",value.animator,&SceneNode::animator,&NodeOverride::animator,[&](auto& c) {
        // The clips of the node's model, once it has been read; otherwise the name as typed.
        const auto* skeleton=value.model ? animations_.find(value.model->path) : nullptr;
        bool edited=false;
        if(skeleton) {
            if(ImGui::BeginCombo("Clip",c.clip.empty()?"(rest pose)":c.clip.c_str())) {
                if(ImGui::Selectable("(rest pose)",c.clip.empty())) c.clip.clear(),edited=true;
                for(const auto& clip:skeleton->clips)
                    if(ImGui::Selectable(clip.name.c_str(),clip.name==c.clip)) c.clip=clip.name,edited=true;
                ImGui::EndCombo();
            }
            track();
        } else edited|=texturePath("Clip",c.clip);
        const float length=skeleton ? skeleton->duration(c.clip) : 0;
        edited|=ImGui::DragFloat("Time (s)",&c.time,.01f,0,length>0?length:1.0e6f,"%.3f",ImGuiSliderFlags_AlwaysClamp); track();
        edited|=ImGui::DragFloat("Speed",&c.speed,.01f,-100,100,"%.2f",ImGuiSliderFlags_AlwaysClamp); track();
        edited|=ImGui::Checkbox("Loop",&c.loop); track();
        ImGui::SameLine();
        edited|=ImGui::Checkbox("Playing",&c.playing); track();
        if(!value.model) ImGui::TextDisabled("Poses the skeleton of the node's Model.");
        else if(!skeleton) ImGui::TextDisabled("The Model has no skeleton (yet).");
        return edited;
    });
    component("Light",value.light,&SceneNode::light,&NodeOverride::light,[&](auto& c) {
        int type=static_cast<int>(c.type);
        bool edited=ImGui::Combo("Type",&type,"Directional\0Point\0Spot\0Area Approx\0Area LTC\0"); track();
        c.type=static_cast<LightType>(type);
        edited|=ImGui::Checkbox("Enabled",&c.enabled); track(); edited|=colour(c);
        edited|=scalar("Power",c.power,0);
        edited|=scalar("Temperature (K)",c.temperature,0,40000);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Tints the colour like a black body: 6500 K is white, lower is warmer, 0 is off.");
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
    // Edits one attachment; @p owned when it is not the prefab's. For the prefab's, @p change is
    // this instance's override of it: what it sets is coloured, with @p item(part) drawing Revert and Apply.
    const auto editAttachment = [&](Behaviour& behaviour, const char* symbol, bool owned, const AttachmentOverride* change,
                                    auto&& item) {
        bool changed = false;
        // Draws one setting, coloured when overridden, then Revert and Apply for it.
        const auto setting = [&](const char* id, bool overridden, auto&& widget, auto&& part) {
            ImGui::PushID(id);
            if (overridden) ImGui::PushStyleColor(ImGuiCol_Text, overriddenColour);
            widget();
            if (overridden) ImGui::PopStyleColor();
            if (overridden) {
                AttachmentOverride piece{change->id};
                part(piece);
                item(std::move(piece));
            }
            ImGui::PopID();
        };
            char buffer[512];
            setting("script", change && change->script, [&] {
                fill(buffer, shown(behaviour.script));
                if (ImGui::InputText("Script", buffer, sizeof buffer)) behaviour.script = assets_.bind(std::string(buffer)), changed = true;
                track();
            }, [&](AttachmentOverride& part) { part.script = change->script; });
            const auto& cached = fieldsOf(behaviour.script, symbol);
            setting("enabled", change && change->enabled, [&] {
                changed|=ImGui::Checkbox("Enabled",&behaviour.enabled); track();
            }, [&](AttachmentOverride& part) { part.enabled = change->enabled; });
            // RenderEvent attachments have no priority overrides.
            if (std::string_view(symbol) == "Behaviour") {
                const auto priority=[&](const char* label,std::optional<double>& value,double inherited) {
                    bool overridden=value.has_value();
                    if(ImGui::Checkbox("Override",&overridden)) { value=overridden?std::optional<double>(inherited):std::nullopt; changed=true; }
                    track(); ImGui::SameLine();
                    if(value) {
                        double number=*value;
                        if(ImGui::InputDouble(label,&number) && std::isfinite(number)) { value=number; changed=true; }
                        track();
                    } else ImGui::TextDisabled("%s: %.9g (inherited)",label,inherited);
                };
                if (ImGui::TreeNode("Priority")) {
                    setting("Priority", change && change->priority, [&] {
                        priority("Priority",behaviour.priority,cached.behaviour.defaults.priority);
                    }, [&](AttachmentOverride& part) { part.priority = change->priority; });
                    for(int phase=0;phase<3;++phase) {
                        auto inherited=behaviour; inherited.phasePriorities[phase].reset();
                        const auto label=std::string(behaviourMethods[phase])+" priority";
                        setting(label.c_str(), change && change->phasePriorities[phase], [&] {
                            priority(label.c_str(),behaviour.phasePriorities[phase],effectivePriority(inherited,cached.behaviour.defaults,phase));
                        }, [&](AttachmentOverride& part) { part.phasePriorities[phase] = change->phasePriorities[phase]; });
                    }
                    ImGui::TreePop();
                }
            }
            if (!cached.error.empty()) ImGui::TextWrapped("%s", cached.error.c_str());
            for (const auto& info : cached.fields) {
                auto found = behaviour.data.find(info.name);
                FieldValue field = found != behaviour.data.end() && found->second.index() == info.defaultValue.index()
                    ? found->second : info.defaultValue;
                bool edited = false;
                setting(info.name.c_str(), change && change->data.count(info.name), [&] {
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
                }, [&](AttachmentOverride& part) { part.data[info.name] = change->data.at(info.name); });
                if (edited) behaviour.data[info.name] = field, changed = true;
                // Reset returns the attachment's own field to the script's default.
                if (owned && found != behaviour.data.end()) {
                    ImGui::PushID(info.name.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset")) {
                        checkpoint();
                        behaviour.data.erase(info.name);
                        changed = true;
                    }
                    ImGui::PopID();
                }
            }
        return changed;
    };
    // Behaviour and RenderEvent use the same component section as the other built-in components.
    // Work on a copy so edits never invalidate the expansion being displayed. Inside an
    // instance, an attachment of the prefab is changed by overrides, field by field (TDDja/07 4);
    // one the instance added is edited where the overrides hold it.
    const auto attachmentSection = [&](const char* title, const char* symbol, std::vector<Behaviour> SceneNode::*list,
                                       AttachmentsOverride NodeOverride::*layered) {
        const auto* overrides = layer ? &(layer->*layered) : nullptr;
        if ((value.*list).empty() && !(overrides && overrides->has_value())) return;
        const auto edit = [&]() -> AttachmentsOverride& { return target.overrides->node(target.relative).*layered; };
        // Revert (or Restore) and Apply for one part of this list's overrides.
        const auto item = [&](AttachmentsOverride part, const char* revert) {
            Overrides piece;
            piece.node(target.relative).*layered = std::move(part);
            ImGui::SameLine();
            if (ImGui::SmallButton(revert)) reverting = piece;
            if (!target.holder) return;
            ImGui::SameLine();
            if (ImGui::SmallButton("Apply")) applying.emplace(target.holder, std::move(piece));
        };
        const auto addedHere = [&](NodeId id) {
            return overrides && std::any_of(overrides->added.begin(), overrides->added.end(), [&](const Behaviour& each) { return each.id == id; });
        };
        componentSection(title,true,layered,[&] {
            ImGui::Indent();
            const auto& listed = value.*list;
            auto attachments = listed;
            bool attachmentsChanged = false;
            // Dragging an attachment's header onto another's moves it to that place.
            const auto payload = std::string("THRONE_ATTACHMENT_") + symbol;
            std::optional<std::pair<std::size_t, std::size_t>> move;
            for (std::size_t k = 0; k < attachments.size(); ++k) {
                auto& behaviour = attachments[k];
                const bool added = addedHere(behaviour.id);
                const AttachmentOverride* change = nullptr;
                if (overrides)
                    for (const auto& entry : overrides->changed)
                        if (entry.id == behaviour.id) change = &entry;
                ImGui::PushID(static_cast<int>(k));
                bool keep = true;
                if (added || change) ImGui::PushStyleColor(ImGuiCol_Text, overriddenColour);
                const bool shownOpen = ImGui::CollapsingHeader((shown(behaviour.script) + "###attachment").c_str(), &keep, open);
                if (added || change) ImGui::PopStyleColor();
                if ((own || target.overrides) && ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload(payload.c_str(), &k, sizeof k);
                    ImGui::TextUnformatted(shown(behaviour.script).c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const auto* dropped = ImGui::AcceptDragDropPayload(payload.c_str()))
                        move.emplace(*static_cast<const std::size_t*>(dropped->Data), k);
                    ImGui::EndDragDropTarget();
                }
                if (added) {
                    AttachmentsOverride part;
                    part.added.push_back(behaviour);
                    item(std::move(part), "Revert");
                }
                if (shownOpen && editAttachment(behaviour, symbol, own || added, change, [&](AttachmentOverride part) {
                        AttachmentsOverride parts;
                        parts.changed.push_back(std::move(part));
                        item(std::move(parts), "Revert");
                    })) {
                    if (own || added) {
                        attachmentsChanged = true;
                    } else if (target.overrides) {
                        // Only what differs from the prefab's attachment becomes an override.
                        const auto& before = listed[k];
                        AttachmentsOverride part;
                        auto& differs = part.change(behaviour.id);
                        if (behaviour.script != before.script) differs.script = behaviour.script;
                        if (behaviour.enabled != before.enabled) differs.enabled = behaviour.enabled;
                        if (behaviour.priority != before.priority) differs.priority = behaviour.priority;
                        for (int phase = 0; phase < 3; ++phase)
                            if (behaviour.phasePriorities[phase] != before.phasePriorities[phase])
                                differs.phasePriorities[phase] = behaviour.phasePriorities[phase];
                        for (const auto& [name, field] : behaviour.data)
                            if (const auto it = before.data.find(name); it == before.data.end() || !(it->second == field))
                                differs.data[name] = field;
                        edit().merge(part);
                    }
                }
                if (!keep) {
                    checkpoint();
                    if (own) {
                        attachments.erase(attachments.begin() + static_cast<std::ptrdiff_t>(k));
                        attachmentsChanged = true;
                    } else if (target.overrides) {
                        AttachmentsOverride part;
                        part.removed.push_back(behaviour.id);
                        edit().merge(part);
                    }
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            // The prefab's attachments this instance removes.
            if (overrides)
                for (const auto id : overrides->removed) {
                    const auto name = view_.instances[selected_.instance].expansion.removedAttachments.find({selected_.path, id});
                    if (name == view_.instances[selected_.instance].expansion.removedAttachments.end()) continue;
                    ImGui::PushID(static_cast<int>(id));
                    ImGui::TextColored(overriddenColour, "Removed %s", shown(name->second).c_str());
                    AttachmentsOverride part;
                    part.removed.push_back(id);
                    item(std::move(part), "Restore");
                    ImGui::PopID();
                }
            if (overrides && !overrides->order.empty()) {
                ImGui::TextColored(overriddenColour, "Order changed");
                AttachmentsOverride part;
                part.order = overrides->order;
                item(std::move(part), "Revert");
            }
            if (move && move->first != move->second && move->first < attachments.size()) {
                checkpoint();
                auto moved = std::move(attachments[move->first]);
                attachments.erase(attachments.begin() + static_cast<std::ptrdiff_t>(move->first));
                attachments.insert(attachments.begin() + static_cast<std::ptrdiff_t>(move->second), std::move(moved));
                if (own) {
                    attachmentsChanged = true;
                } else if (target.overrides) {
                    // An instance records the whole order, its own attachments among the prefab's.
                    AttachmentsOverride part;
                    for (const auto& each : attachments) part.order.push_back(each.id);
                    edit().merge(part);
                }
            }
            if (ImGui::Button("Add Script")) {
                checkpoint();
                if (own) attachments.push_back({}), attachmentsChanged = true;
                else if (target.overrides) edit().added.push_back({});
            }
            ImGui::Unindent();
            if (!attachmentsChanged) return;
            if (own) {
                own->*list = std::move(attachments);
            } else if (target.overrides) {
                // Attachments this instance added are kept in its overrides, edited there.
                for (auto& each : edit().added)
                    for (const auto& edited : attachments)
                        if (edited.id == each.id) each = edited;
            }
        },[&] {
            if (own) {
                (own->*list).clear();
            } else if (target.overrides) {
                AttachmentsOverride part;
                for (const auto& each : value.*list)
                    if (!addedHere(each.id)) part.removed.push_back(each.id);
                auto& into = edit();
                into.added.clear();
                into.merge(part);
            }
        });
    };
    attachmentSection("Behaviour", "Behaviour", &SceneNode::behaviour, &NodeOverride::behaviour);
    attachmentSection("Render Event", "RenderEvent", &SceneNode::renderEvent, &NodeOverride::renderEvent);
    // The system fields that refer to this node; clicking one selects its system.
    const auto& nodes = view_.instances[selected_.instance].expansion.nodes;
    if (const auto found = references_.find({static_cast<std::size_t>(selected_.instance), &expanded - nodes.data()});
        found != references_.end()) {
        ImGui::Separator();
        ImGui::TextUnformatted("Referenced by");
        for (std::size_t i = 0; i < found->second.size(); ++i) {
            const auto referrer = found->second[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(referrer.label.c_str()))
                selected_ = {referrer.system ? Selection::Kind::System : Selection::Kind::Node,
                             referrer.instance, referrer.from, referrer.system};
            ImGui::PopID();
        }
    }

    if (!expanded.node.prefab.empty()) {
        ImGui::Separator();
        ImGui::Text("Instance of %s", shown(expanded.node.prefab).c_str());
        if (ImGui::Button("Open Prefab")) enter({Context::Kind::Prefab, expanded.node.prefab});
        // Only an instance the edited file holds itself can be unpacked into it.
        if (target.node && !target.addedTo) {
            ImGui::SameLine();
            if (ImGui::Button("Unpack")) unpack(selected_.instance, target.node->id);
        }
        const auto& expansion = view_.instances[selected_.instance].expansion;
        if (target.node && target.holder == target.node) {
            // Writes every override of this instance into the prefab (as dropping it on the file does).
            ImGui::SameLine();
            ImGui::BeginDisabled(target.node->overrides.empty());
            if (ImGui::Button("Apply All")) applying.emplace(target.node, target.node->overrides);
            ImGui::SameLine();
            if (ImGui::Button("Revert All")) {
                checkpoint();
                target.node->overrides = {};
            }
            ImGui::EndDisabled();
            if (expansion.unsure(selected_.path))
                ImGui::TextWrapped("The prefab cannot be read: its overrides are kept until it can be again.");
            if (auto piece = overridesList(*target.overrides, expansion, selected_.path, true))
                applying.emplace(target.node, std::move(*piece));
        }
    }
    if (own || target.overrides || target.node) ImGui::Separator();
    if ((own || target.overrides) && ImGui::BeginCombo("##add", "Add Component")) {
        if (value.behaviour.empty() && ImGui::Selectable("Behaviour")) {
            checkpoint();
            if (own) own->behaviour.push_back({});
            else target.overrides->node(target.relative).behaviour.added.push_back({});
        }
        if (value.renderEvent.empty() && ImGui::Selectable("Render Event")) {
            checkpoint();
            if (own) own->renderEvent.push_back({});
            else target.overrides->node(target.relative).renderEvent.added.push_back({});
        }
        if (own) {
            if (!own->transform && ImGui::Selectable("Transform")) checkpoint(), own->transform.emplace();
            const auto addPrimitive=[&](const char* label,PrimitiveKind kind) {
                if (primitiveShape(*own).kind!=kind && ImGui::Selectable(label)) checkpoint(),setPrimitive(*own,kind);
            };
            addPrimitive("Box Primitive",PrimitiveKind::Box); addPrimitive("Sphere Primitive",PrimitiveKind::Sphere);
            addPrimitive("Capsule Primitive",PrimitiveKind::Capsule); addPrimitive("Plane Primitive",PrimitiveKind::Plane);
            if(!own->particles && ImGui::Selectable("Particle System")) checkpoint(),own->particles.emplace();
            if(!own->model && ImGui::Selectable("Model")) checkpoint(),own->model.emplace();
            if(!own->camera && ImGui::Selectable("Camera")) checkpoint(),own->camera.emplace();
            if(!own->animator && ImGui::Selectable("Animator")) checkpoint(),own->animator.emplace();
            if(!own->audioSource && ImGui::Selectable("Audio Source")) checkpoint(),own->audioSource.emplace();
            if(!own->audioListener && ImGui::Selectable("Audio Listener")) checkpoint(),own->audioListener.emplace();
            if(!own->acousticGeometry && ImGui::Selectable("Acoustic Geometry")) checkpoint(),own->acousticGeometry.emplace();
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
        }
        ImGui::EndCombo();
    }

    if (target.node) {
        if (!target.addedTo && ImGui::Button("Duplicate")) duplicate();
        if (!target.addedTo) ImGui::SameLine();
        if (ImGui::Button("Delete")) erase();
        // A node an instance added joins the prefab.
        if (target.addedBy) {
            ImGui::SameLine();
            if (ImGui::Button("Apply")) {
                Overrides piece;
                for (const auto& entry : target.addedTo->added)
                    if (entry.node.id == target.node->id) piece.added.push_back(entry);
                applying.emplace(target.addedBy, std::move(piece));
            }
        }
    }
    if (reverting) {
        checkpoint();
        target.overrides->subtract(*reverting);
    }
    if (applying) applyOverrides(*applying->first, std::move(applying->second));
}

void Editor::levelInspector() {
    ImGui::SeparatorText("Audio");
    char mixer[256]; fill(mixer,doc_.world.level.mixer.empty()?std::string():shown(doc_.world.level.mixer));
    if(ImGui::InputText("Mixer",mixer,sizeof mixer,ImGuiInputTextFlags_EnterReturnsTrue)) {
        try {
            const std::string path=mixer;
            if(path.empty()) { checkpoint(); doc_.world.level.mixer.clear(); }
            else { openMixer(path); checkpoint(); doc_.world.level.mixer=assets_.bind(path); }
        } catch(const std::exception& e) { log(e.what()); }
    }
    if(!doc_.world.level.mixer.empty() && ImGui::Button("Edit Mixer")) {
        try { openMixer(doc_.world.level.mixer); } catch(const std::exception& e) { log(e.what()); }
    }

    ImGui::TextUnformatted(std::filesystem::u8path(levelPath_).filename().u8string().c_str());
    ImGui::SeparatorText("Rendering");
    auto& rendering=doc_.world.level.rendering;
    bool clustered=rendering.clustered;
    if(ImGui::Checkbox("Forward Clustered",&clustered)) { checkpoint(); rendering.clustered=clustered; }
    ImGui::SameLine();
    bool clusters=rendering.debugClusters;
    if(ImGui::Checkbox("Show Clusters",&clusters)) { checkpoint(); rendering.debugClusters=clusters; }
    float range[2]={rendering.minDistance,rendering.maxDistance};
    const bool rangeEdited=ImGui::DragFloat2("Forward+ Depth Range",range,.1f,.001f,1.0e6f,"%.2f",ImGuiSliderFlags_AlwaysClamp);
    if(ImGui::IsItemActivated()) editStart_=doc_;
    if(rangeEdited) {
        rendering.minDistance=range[0];
        rendering.maxDistance=(std::max)(range[1],range[0]*1.01f);
    }
    if(ImGui::IsItemDeactivatedAfterEdit()) commit(std::move(editStart_));
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("The depth range Forward+ divides into cells for local lights.");
    // The view's camera, edited here as by navigating; it is not saved with the level.
    ImGui::SeparatorText("Scene View Camera (not saved)");
    const Ogre::Vector3 offset(std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_), std::cos(pitch_) * std::cos(yaw_));
    auto position = fromOgre(target_ + offset * distance_);
    if (ImGui::DragFloat3("Position", position.data(), .05f, -1.0e6f, 1.0e6f, "%.3f")) {
        target_ = toOgre(position) - offset * distance_;
        updateCamera();
    }
    // As a Transform's rotation: about X (looking down is negative), then Y; no roll.
    float rotation[2] = {-Ogre::Radian(pitch_).valueDegrees(), Ogre::Radian(yaw_).valueDegrees()};
    if (ImGui::DragFloat2("Rotation X / Y", rotation, .5f, -1.0e6f, 1.0e6f, "%.1f deg")) {
        const auto at = target_ + offset * distance_;
        pitch_ = std::clamp(-Ogre::Degree(rotation[0]).valueRadians(), -1.5f, 1.5f);
        yaw_ = Ogre::Degree(rotation[1]).valueRadians();
        const Ogre::Vector3 turned(std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_), std::cos(pitch_) * std::cos(yaw_));
        target_ = at - turned * distance_;
        updateCamera();
    }
    if (ImGui::DragFloat("Field of View", &viewFieldOfView_, .5f, 1, 170, "%.1f deg", ImGuiSliderFlags_AlwaysClamp)) updateCamera();
    if (ImGui::DragFloat("Near Clip", &viewNear_, .01f, .001f, 1.0e6f, "%.3f", ImGuiSliderFlags_AlwaysClamp) |
        ImGui::DragFloat("Far Clip", &viewFar_, 1, .01f, 1.0e7f, "%.1f", ImGuiSliderFlags_AlwaysClamp)) {
        viewFar_ = (std::max)(viewFar_, viewNear_ * 1.001f);
        updateCamera();
    }
    ImGui::DragFloat("Focus Distance", &viewFocusDistance_, .05f, .01f, 1.0e6f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
    ImGui::DragFloat("F-Stop", &viewFStop_, .1f, 0, 64, viewFStop_ > 0 ? "f/%.1f" : "off", ImGuiSliderFlags_AlwaysClamp);
    antiAliasingCombo(viewAntiAliasing_);
    ImGui::SeparatorText("Scene View Audio Listener (not saved)");
    ImGui::Checkbox("Enabled##SceneViewAudioListener",&viewAudioListener_);
    ImGui::TextDisabled("Follows Scene View Camera; highest priority during Play.");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("While Scene View is visible, overrides every scene listener.\nDisable to use scene AudioListener priorities.");
}

bool Editor::antiAliasingCombo(AntiAliasing& mode) {
    int index = static_cast<int>(mode);
    const bool changed = ImGui::Combo("Anti-Aliasing", &index, "TAA\0SMAA\0None\0");
    if (changed) mode = static_cast<AntiAliasing>(index);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("TAA: jittered frames blended over time; alpha hashing and the noise of SSAO, SSR and\n"
                          "depth of field average out (orthographic views use SMAA).\n"
                          "SMAA: edges of each frame alone; that noise stays visible.\nNone: for comparison.");
    return changed;
}

void Editor::sceneInspector(SceneInstance& instance) {
    const auto track = [this] {
        if (ImGui::IsItemActivated()) editStart_ = doc_;
        if (ImGui::IsItemDeactivatedAfterEdit()) commit(std::move(editStart_));
    };
    ImGui::TextDisabled("Scene instance #%u", instance.id);
    ImGui::TextUnformatted(shown(instance.scene).c_str());
    auto& transform = instance.transform;
    axisDrag3("Position", transform.position.data(), 0.05f, -1.0e6f, 1.0e6f, "%.3f");
    track();
    axisDrag3("Rotation", transform.rotation.data(), 0.5f, -1.0e6f, 1.0e6f, "%.1f");
    track();
    axisDrag3("Scale", transform.scale.data(), 0.01f, 0.001f, 1.0e6f, "%.3f");
    track();
    ImGui::Separator();
    ImGui::Separator();
    if (ImGui::Button("Open Scene")) enter({Context::Kind::Scene, instance.scene});
    ImGui::SameLine();
    if (ImGui::Button("Remove")) removeScene(instance.id);
    // The scene's own settings, written to its file.
    lightingInspector(instance.scene);
}

std::optional<Overrides> Editor::overridesList(Overrides& overrides, const Expansion& expansion, const NodePath& prefix,
                                               bool apply) {
    std::optional<Overrides> applied;
    std::vector<Overrides> dropped;
    // Restore and Apply for one removal; one that names nothing is listed below instead.
    const auto removal = [&](const char* label, const std::string* name, Overrides piece) {
        if (!name) return;
        ImGui::TextColored(overriddenColour, "%s %s", label, name->c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Restore")) dropped.push_back(piece);
        if (!apply) return;
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply")) applied = std::move(piece);
    };
    int id = 0;
    for (const auto& path : overrides.removed) {
        auto where = prefix;
        where.insert(where.end(), path.begin(), path.end());
        const auto name = expansion.removed.find(where);
        Overrides piece;
        piece.removed.push_back(path);
        ImGui::PushID(id++);
        removal("Removed", name != expansion.removed.end() ? &name->second : nullptr, std::move(piece));
        ImGui::PopID();
    }
    for (const auto& entry : overrides.removedSystems) {
        auto where = prefix;
        where.insert(where.end(), entry.instance.begin(), entry.instance.end());
        const auto name = expansion.removedSystems.find({where, entry.system});
        Overrides piece;
        piece.removedSystems.push_back(entry);
        ImGui::PushID(id++);
        removal("Removed system", name != expansion.removedSystems.end() ? &name->second : nullptr, std::move(piece));
        ImGui::PopID();
    }
    // Overrides of what the prefab no longer has stay until removed here (TDDja/06 4.2).
    std::vector<const UnusedOverride*> unused;
    for (const auto& entry : expansion.unused)
        if (entry.instance == prefix) unused.push_back(&entry);
    if (!unused.empty()) {
        ImGui::TextDisabled("Overrides of what the prefab no longer has:");
        if (unused.size() > 1) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove All"))
                for (const auto* entry : unused) dropped.push_back(entry->piece);
        }
        for (const auto* entry : unused) {
            ImGui::PushID(id++);
            ImGui::TextDisabled("%s", entry->label.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) dropped.push_back(entry->piece);
            ImGui::PopID();
        }
    }
    if (!dropped.empty()) {
        checkpoint();
        for (const auto& piece : dropped) overrides.subtract(piece);
    }
    return applied;
}

void Editor::systemInspector(const System& system) {
    const auto track = [this] {
        if (ImGui::IsItemActivated()) editStart_ = doc_;
        if (ImGui::IsItemDeactivatedAfterEdit()) commit(std::move(editStart_));
    };
    auto target = systemTarget();
    ImGui::TextDisabled(target.addedTo  ? "Added by an instance: kept in its overrides"
                        : target.system ? "System"
                                        : "System of an instance: field edits are overrides");
    char text[256];
    if (target.system) {
        fill(text, target.system->name);
        if (ImGui::InputText("Name", text, sizeof text) && text[0]) target.system->name = text;
        track();
    } else {
        ImGui::Text("%s", system.name.c_str());
    }
    // The layer field values are written to: the system itself, or this instance's overrides.
    const auto layer = [&](std::size_t k) -> std::map<std::string, FieldValue>* {
        if (target.system) return &target.system->behaviour[k].data;
        if (target.overrides) return &target.overrides->fieldsOf(target.instance, selected_.system, k).fields;
        return nullptr;
    };
    const auto overridden = [&](std::size_t k, const std::string& name) {
        if (target.system) return target.system->behaviour[k].data.count(name) > 0;
        if (!target.overrides) return false;
        for (const auto& entry : target.overrides->systems)
            if (entry.instance == target.instance && entry.system == selected_.system && entry.behaviour == k)
                return entry.fields.count(name) > 0;
        return false;
    };

    std::optional<std::size_t> remove;
    // A field override to write into the instance's prefab, once nothing here points into it.
    std::optional<Overrides> applying;
    for (std::size_t k = 0; k < system.behaviour.size(); ++k) {
        const auto& behaviour = system.behaviour[k];
        ImGui::PushID(static_cast<int>(k));
        const auto title = (behaviour.script.empty() ? std::string("(no script)") : shown(behaviour.script)) + "###behaviour";
        if (ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (target.system) {
                fill(text, shown(behaviour.script));
                if (ImGui::InputTextWithHint("Script", "scripts/....lh", text, sizeof text))
                    target.system->behaviour[k].script = assets_.bind(std::string(text));
                track();
            }
            const auto& cached = fieldsOf(behaviour.script, "Behaviour");
            if (!cached.error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", cached.error.c_str());
            for (const auto& info : cached.fields) {
                ImGui::PushID(info.name.c_str());
                const auto found = behaviour.data.find(info.name);
                // A stored value of the wrong type (the script changed) shows the default.
                const FieldValue value = found != behaviour.data.end() && found->second.index() == info.defaultValue.index()
                                             ? found->second
                                             : info.defaultValue;
                const bool marked = overridden(k, info.name);
                if (marked && !target.system) ImGui::PushStyleColor(ImGuiCol_Text, overriddenColour);
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
                    // A reference without a scene is relative to where the system is; one
                    // with a scene instance ID (level systems) starts at that scene.
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
                        // A level system may pick any node; any other system, nodes of its own instance.
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
                if (marked && !target.system) ImGui::PopStyleColor();
                if (marked) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(target.system ? "Reset" : "Revert")) {
                        checkpoint();
                        if (auto* fields = layer(k)) fields->erase(info.name);
                    }
                    if (target.holder) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Apply")) {
                            Overrides piece;
                            piece.systems.push_back({target.instance, selected_.system, k, {{info.name, (*layer(k))[info.name]}}});
                            applying = std::move(piece);
                        }
                    }
                }
                ImGui::PopID();
            }
            if (target.system && ImGui::Button("Remove Behaviour")) remove = k;
        }
        ImGui::PopID();
    }
    if (applying) applyOverrides(*target.holder, std::move(*applying));
    if (!target.system) return;
    if (remove) {
        checkpoint();
        target.system->behaviour.erase(target.system->behaviour.begin() + static_cast<std::ptrdiff_t>(*remove));
    }
    ImGui::Separator();
    if (ImGui::Button("Add Behaviour")) {
        checkpoint();
        target.system->behaviour.push_back({});
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete System")) erase();
    // A system an instance added joins the prefab.
    if (target.addedBy) {
        ImGui::SameLine();
        if (ImGui::Button("Apply")) {
            Overrides piece;
            piece.addedSystems.push_back(*target.system);
            applyOverrides(*target.addedBy, std::move(piece));
        }
    }
}

const Editor::CachedFields& Editor::fieldsOf(const AssetRef& ref, const std::string& symbol) {
    const auto script = assets_.path(ref);
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
        if(symbol=="Behaviour") {
            entry.behaviour=ScriptRuntime::inspectBehaviour(project_->root,script);
            entry.fields=entry.behaviour.fields;
        } else entry.fields = ScriptRuntime::fields(project_->root, script, symbol);
        entry.error.clear();
    } catch (const std::exception& failure) {
        entry.fields.clear();
        entry.error = failure.what();
    }
    return entry;
}
}
