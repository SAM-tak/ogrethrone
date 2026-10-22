#include "editor/Editor.h"
#include <imgui_internal.h>
#include <array>
#include <cmath>
#include <cstdio>

namespace throne {
/// Runs against main.cpp's throwaway copy of samples/audio.
void audioEditorSmoke(Editor& editor) {
    const auto require=[](bool b,const char* message){if(!b) throw std::runtime_error(message);};
    const auto frame=[&]{require(editor.frame(float(fixedStep)),"Audio editor frame stopped");};
    auto& e=editor;
    require(e.project_.has_value(),"Audio sample did not open");
    e.openMixer("mixers/main.mixer.lton");
    for(int i=0;i<3;++i) frame();
    const auto* panel=ImGui::FindWindowByName("Mixer"); require(panel && panel->Active,"Mixer panel not rendered");
    const auto scene=e.assets_.bind(std::string("scenes/main.scene.lton"));
    const auto gain=[&]{return e.doc_.world.scenes.at(scene).nodes[1].audioSource->volume;};
    e.checkpoint(); e.doc_.world.scenes.at(scene).nodes[1].audioSource->volume=.25f;
    e.undo(); require(gain()==1,"Source undo failed");
    e.redo(); require(gain()==.25f,"Source redo failed");
    e.checkpoint(); e.doc_.world.scenes.at(scene).nodes[1].audioSource->reflectionVolume=.4f;
    e.undo(); require(e.doc_.world.scenes.at(scene).nodes[1].audioSource->reflectionVolume==.7f,"Reflection volume undo failed");
    e.redo(); require(e.doc_.world.scenes.at(scene).nodes[1].audioSource->reflectionVolume==.4f,"Reflection volume redo failed");
    e.checkpoint(); e.doc_.mixers.at(e.mixerPath_).buses[2].volume=.5f;
    e.undo(); require(e.doc_.mixers.at(e.mixerPath_).buses[2].volume==1,"Mixer undo failed");
    e.redo(); require(e.doc_.mixers.at(e.mixerPath_).buses[2].volume==.5f,"Mixer redo failed");
    e.checkpoint(); e.doc_.world.scenes.at(scene).nodes[4].acousticGeometry->transmission.x=.4f;
    e.undo(); require(e.doc_.world.scenes.at(scene).nodes[4].acousticGeometry->transmission.x==.25f,"Acoustic material undo failed");
    e.redo(); require(e.doc_.world.scenes.at(scene).nodes[4].acousticGeometry->transmission.x==.4f,"Acoustic material redo failed");
    require(e.save(),"Audio document save failed");
    require(Scene::load(e.mediaFile(scene)).nodes[4].acousticGeometry->transmission.x==.4f,"Acoustic material edit not saved");
    require(Scene::load(e.mediaFile(scene)).nodes[1].audioSource->volume==.25f,"Source edit not saved");
    require(Scene::load(e.mediaFile(scene)).nodes[1].audioSource->reflectionVolume==.4f,"Reflection edit not saved");
    require(MixerDefinition::load(e.mediaFile(e.mixerPath_)).buses[2].volume==.5f,"Mixer edit not saved");
    e.selected_={Editor::Selection::Kind::Level}; frame();
    require(e.viewAudioListener_,"Scene view listener should default to enabled");
    // Render both new component inspectors.
    frame(); e.selected_={Editor::Selection::Kind::Node,0,e.view_.instances[0].expansion.nodes[1].path}; frame();
    e.selected_={Editor::Selection::Kind::Node,0,e.view_.instances[0].expansion.nodes[0].path}; frame();
    e.selected_={Editor::Selection::Kind::Node,0,e.view_.instances[0].expansion.nodes[4].path}; frame();
    e.play(); require(e.playing() && e.audioWorld_,"Editor audio Play refused");
    for(int i=0;i<8;++i) frame();
    require(e.audioWorld_->playing(0,1),"Editor did not autoplay scene audio");
    std::array<float,1024> pcm;
    for(int i=0;i<8;++i) e.audioDevice_->mixer().mix(pcm.data(),512);
    e.togglePause(); require(e.paused() && !e.audioWorld_->playing(0,1),"Editor audio Pause failed");
    frame(); e.togglePause(); frame(); require(e.audioWorld_->playing(0,1),"Editor audio Resume failed");
    e.checkpoint(); e.doc_.mixers.at(e.mixerPath_).buses[0].mute=true; frame();
    double energy=0;
    for(int i=0;i<32;++i) {
        e.audioDevice_->mixer().mix(pcm.data(),512);
        if(i>12) for(float v:pcm) energy+=double(v)*v;
    }
    require(energy<1.0e-10,"Live Mixer edit was not applied at frame boundary");
    e.undo(); frame(); require(!e.doc_.mixers.at(e.mixerPath_).buses[0].mute,"Live Mixer undo failed");
    e.stop(); require(!e.playing() && !e.audioWorld_ && e.audioDevice_->mixer().getVoiceCount()==0,"Editor Stop leaked audio");
    frame(); require(gain()==.25f,"Stop did not restore authored source state");
    e.play(); frame(); require(e.audioWorld_ && e.audioWorld_->playing(0,1),"Second Play failed");
    auto& liveSource=*e.view_.instances[0].expansion.nodes[1].node.audioSource;
    liveSource.clip="audio/generated/noise.wav"; liveSource.occlusion=false; liveSource.reflections=false;
    e.audioWorld_->command(0,1,AudioCommand::Play);
    const auto oldTarget=e.target_; e.target_={10000,0,0};
    const auto outputEnergy=[&] {
        for(int i=0;i<3;++i) frame();
        double sum=0;
        for(int i=0;i<32;++i) {
            e.audioDevice_->mixer().mix(pcm.data(),512);
            if(i>12) for(float v:pcm) sum+=double(v)*v;
        }
        return sum;
    };
    require(outputEnergy()<1.0e-10,"Scene view listener did not follow distant camera");
    e.viewAudioListener_=false;
    require(outputEnergy()>1.0e-8,"Disabling scene view listener did not restore scene listener");
    e.viewAudioListener_=true; e.target_=oldTarget;
    require(outputEnergy()>1.0e-8,"Moving scene view camera did not restore spatial sound");
    e.stop();
    std::puts("Audio editor smoke passed: panels, acoustic geometry, save, undo/redo, Play, Pause, live Mixer, Stop");
}
}
