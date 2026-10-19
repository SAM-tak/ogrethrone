#include "editor/Tasks.h"
#include <imgui.h>
#include <cmath>

namespace throne {
namespace {
/// Draws a ring of dots whose bright head turns once a second.
void spinner(float radius) {
    const auto origin = ImGui::GetCursorScreenPos();
    const ImVec2 centre(origin.x + radius, origin.y + radius);
    auto* draw = ImGui::GetWindowDrawList();
    constexpr int dots = 10;
    const float turn = float(ImGui::GetTime()) * 2 * 3.14159265f;
    for (int i = 0; i < dots; ++i) {
        const float angle = turn + float(i) * 2 * 3.14159265f / dots;
        // The dots fade behind the head.
        const float fade = float(i + 1) / dots;
        draw->AddCircleFilled(ImVec2(centre.x + std::cos(angle) * radius * .75f, centre.y + std::sin(angle) * radius * .75f),
                              radius * .16f, ImGui::GetColorU32(ImGuiCol_Text, fade));
    }
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
}
}

void Tasks::add(std::string label, std::function<bool()> step, Progress progress, std::function<std::string()> detail) {
    (updating_ ? added_ : tasks_).push_back({std::move(label), std::move(step), std::move(progress), std::move(detail), 0});
}

void Tasks::update() {
    // A step may add tasks; they join after the loop, so no step moves while it runs.
    updating_ = true;
    for (std::size_t i = 0; i < tasks_.size();) {
        const bool done = tasks_[i].shown >= 2 && tasks_[i].step();
        if (!done) ++i;
        else tasks_.erase(tasks_.begin() + std::ptrdiff_t(i));
    }
    updating_ = false;
    for (auto& task : added_) tasks_.push_back(std::move(task));
    added_.clear();
}

void Tasks::draw() {
    if (tasks_.empty()) return;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * .5f, viewport->Pos.y + viewport->Size.y * .5f),
                            ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(440, 0));
    ImGui::Begin("Working", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
    for (auto& task : tasks_) {
        ImGui::PushID(&task);
        const float share = task.progress ? task.progress() : -1.0f;
        if (share < 0) {
            spinner(ImGui::GetTextLineHeight() * .5f);
            ImGui::SameLine();
            ImGui::TextUnformatted(task.label.c_str());
        } else {
            ImGui::TextUnformatted(task.label.c_str());
            ImGui::ProgressBar(share, ImVec2(-1, 0));
        }
        if (task.detail) ImGui::TextDisabled("%s", task.detail().c_str());
        ImGui::PopID();
        ++task.shown;
    }
    ImGui::End();
}
}
