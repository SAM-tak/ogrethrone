#include "editor/Editor.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace throne {
namespace {
void mixerPath(const std::string& path) {
    const auto file=std::filesystem::u8path(path);
    constexpr std::string_view suffix=".mixer.lton";
    if(path.size()<=suffix.size() || path.size()>255 || path.find('\0')!=std::string::npos || file.is_absolute() || file.has_root_name() ||
       path.compare(path.size()-suffix.size(),suffix.size(),suffix)!=0)
        throw std::runtime_error("Use a project-relative *.mixer.lton path");
    for(const auto& part:file) if(part=="..") throw std::runtime_error("Mixer path escapes project");
}
}
void Editor::openMixer(const std::string& path) {
    mixerPath(path);
    if(!doc_.mixers.count(path)) doc_.mixers.emplace(path,MixerDefinition::load(project_->resolve(path)));
    mixerPath_=path;
}
void Editor::audioBrowser() {
    if(!ImGui::BeginTabItem("Mixers")) return;
    std::set<std::string> paths;
    std::error_code error;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(project_->resolve("mixers"),error)) {
        if(!entry.is_regular_file()) continue;
        const auto path=entry.path().lexically_relative(project_->root).generic_u8string();
        if(path.size()>11 && path.compare(path.size()-11,11,".mixer.lton")==0) paths.insert(path);
    }
    for(const auto& [path,mixer]:doc_.mixers) paths.insert(path);
    for(const auto& path:paths) if(ImGui::Selectable(path.c_str(),path==mixerPath_)) {
        try { openMixer(path); } catch(const std::exception& e) { log(e.what()); }
    }
    ImGui::InputText("New Mixer",newMixerPath_,sizeof newMixerPath_);
    if(ImGui::Button("Create Mixer")) {
        try {
            const std::string path=newMixerPath_; mixerPath(path);
            if(doc_.mixers.count(path) || std::filesystem::exists(project_->resolve(path)))
                throw std::runtime_error("Mixer already exists: "+path);
            checkpoint(); doc_.mixers.emplace(path,MixerDefinition::defaults()); mixerPath_=path;
        } catch(const std::exception& e) { log(e.what()); }
    }
    ImGui::EndTabItem();
}
void Editor::mixerPanel() {
    if(mixerPath_.empty()) return;
    const auto at=doc_.mixers.find(mixerPath_); if(at==doc_.mixers.end()) return;
    bool open=true;
    if(ImGui::Begin("Mixer",&open)) {
        ImGui::TextUnformatted(mixerPath_.c_str());
        ImGui::TextDisabled("%s",dirty_?"Unsaved changes":"Saved");
        if(ImGui::Button("Save")) save();
        ImGui::SameLine();
        ImGui::BeginDisabled(runtime_!=nullptr);
        if(ImGui::Button("Use for Level")) { checkpoint(); doc_.world.level.mixer=mixerPath_; }
        ImGui::EndDisabled();
        auto& definition=at->second;
        std::uint32_t remove=0;
        for(auto& bus:definition.buses) {
            ImGui::PushID(static_cast<int>(bus.id));
            ImGui::SeparatorText(bus.name.c_str());
            ImGui::TextDisabled("ID %u",bus.id);
            if(bus.id!=1) {
                char name[256]; std::snprintf(name,sizeof name,"%s",bus.name.c_str());
                const bool renamed=ImGui::InputText("Name",name,sizeof name);
                if(ImGui::IsItemActivated()) editStart_=doc_;
                if(renamed && name[0]) bus.name=name;
                if(ImGui::IsItemDeactivatedAfterEdit()) commit(std::move(editStart_));
                std::string parent;
                for(const auto& b:definition.buses) if(b.id==bus.parent) parent=b.name;
                if(ImGui::BeginCombo("Parent",parent.c_str())) {
                    for(const auto& b:definition.buses) if(b.id!=bus.id) {
                        auto candidate=definition;
                        for(auto& c:candidate.buses) if(c.id==bus.id) c.parent=b.id;
                        bool valid=true;
                        try { candidate.validate(); } catch(...) { valid=false; }
                        ImGui::BeginDisabled(!valid);
                        if(ImGui::Selectable(b.name.c_str(),bus.parent==b.id)) { checkpoint(); bus.parent=b.id; }
                        ImGui::EndDisabled();
                    }
                    ImGui::EndCombo();
                }
            }
            float volume=bus.volume;
            const bool changed=ImGui::DragFloat("Volume",&volume,.01f,0,100,"%.3f",ImGuiSliderFlags_AlwaysClamp);
            if(ImGui::IsItemActivated()) editStart_=doc_;
            if(changed) bus.volume=volume;
            if(ImGui::IsItemDeactivatedAfterEdit()) commit(std::move(editStart_));
            bool mute=bus.mute;
            if(ImGui::Checkbox("Mute",&mute)) { checkpoint(); bus.mute=mute; }
            if(bus.id!=1 && ImGui::Button("Delete Bus")) remove=bus.id;
            ImGui::PopID();
        }
        if(remove) {
            checkpoint();
            for(auto& bus:definition.buses) if(bus.parent==remove) bus.parent=1;
            definition.buses.erase(std::remove_if(definition.buses.begin(),definition.buses.end(),
                [&](const auto& b){return b.id==remove;}),definition.buses.end());
        }
        if(ImGui::Button("Add Bus")) {
            if(definition.nextBusId<=UINT32_MAX) {
                checkpoint(); const auto id=static_cast<std::uint32_t>(definition.nextBusId++);
                definition.buses.push_back({id,1,"Bus "+std::to_string(id)});
            }
        }
    }
    ImGui::End();
    if(!open) mixerPath_.clear();
}
}
