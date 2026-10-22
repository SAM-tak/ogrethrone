#include "audio/AudioWorld.h"
#include "audio/SpatialSound.h"
#include "script/ScriptRuntime.h"
#include <soloud_wav.h>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>

namespace {
void require(bool yes,const char* message) { if(!yes) throw std::runtime_error(message); }
template<class F> void rejects(F call,const char* message) {
    bool rejected=false; try { call(); } catch(const std::exception&) { rejected=true; }
    require(rejected,message);
}
std::array<double,2> render(throne::AudioSystem& audio,int blocks=32) {
    std::array<float,1024> output;
    std::array<double,2> energy{};
    for(int b=0;b<blocks;++b) {
        audio.mixer().mix(output.data(),512);
        // Discard transition blocks (SoLoud volume ramps, HRTF history).
        if(b<12) continue;
        for(unsigned i=0;i<512;++i) for(unsigned c=0;c<2;++c) {
            const auto v=output[2*i+c]; require(std::isfinite(v),"Nonfinite scene audio"); energy[c]+=double(v)*v;
        }
    }
    return energy;
}
double total(std::array<double,2> e) { return e[0]+e[1]; }
}
int main(int argc,char** argv) {
    using namespace throne;
    namespace fs=std::filesystem;
    try {
        const fs::path root=fs::path(THRONE_AUDIO_SAMPLE)/"assets";
        const bool device=argc>1 && std::string(argv[1])=="--device";
        AudioSystem audio(!device);
        for(const char* file:{"audio/kenney/impact-sounds/footstep_carpet_000.ogg","audio/kenney/interface-sounds/back_001.ogg","audio/kenney/impact-sounds/footstep_carpet_000-mono.wav"}) {
            SoLoud::Wav wave;
            require(wave.load((root/file).u8string().c_str())==SoLoud::SO_NO_ERROR,"Cannot decode Kenney clip");
            require(wave.getLength()>0 && wave.getLength()<5,"Kenney fixture must be short");
            double energy=0; for(unsigned i=0;i<wave.mSampleCount*wave.mChannels;++i) energy+=double(wave.mData[i])*wave.mData[i];
            require(energy>1.0e-6,"Silent Kenney fixture");
            std::printf("%s: %u channels, %.3f s\n",file,wave.mChannels,wave.getLength());
            if(std::string(file).find("-mono.wav")!=std::string::npos) require(wave.mChannels==1,"3D Kenney fixture must be mono");
        }
        auto mixer=MixerDefinition::load(root/"mixers/main.mixer.lton");
        AssetDatabase assets(root,{}); assets.scan([](const std::string&){});
        World world; world.level=Level::load(root/"levels/main.level.lton");
        for(const auto& instance:world.level.scenes) world.scenes.emplace(instance.scene,Scene::load(root/fs::u8path(assets.path(instance.scene))));
        Library library(root,&assets); auto expanded=expand(world,library);
        // The positioning/mixer checks isolate the direct sound; the device demo keeps its walls.
        if(!device) for(auto& instance:expanded.instances) for(auto& node:instance.expansion.nodes)
            {
                if(node.node.acousticGeometry) node.node.acousticGeometry->enabled=false;
                if(node.node.audioSource) node.node.audioSource->reflections=false;
            }
        std::vector<std::string> warnings;
        AudioWorld runtime(audio,root,mixer,[&](const std::string& s){warnings.push_back(s);},&assets);
        ScriptRuntime scripts(world,expanded,root.parent_path(),{},PlayScope::Level,&assets);
        scripts.setAudioCallbacks([&](auto i,auto n,auto c){runtime.command(i,n,c);},[&](auto i,auto n){return runtime.playing(i,n);});
        const auto tick=[&] {
            runtime.spatialStep(expanded); runtime.step(expanded);
        };
        if(device) {
            for(int i=0;i<180;++i) {
                if(i==60) runtime.setPaused(true);
                if(i==80) runtime.setPaused(false);
                scripts.update(float(fixedStep),[&](const auto& stage) {
                    if(stage=="SteamAudio") runtime.spatialStep(expanded);
                    if(stage=="SoLoud") runtime.step(expanded);
                });
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            require(runtime.playing(0,1),"Device loop did not survive pause/resume");
            runtime.clear(); std::puts("Scene audio device playback completed"); return 0;
        }
        auto& nodes=expanded.instances[0].expansion.nodes;
        nodes[2].node.audioSource->autoPlay=false;
        auto& source=*nodes[1].node.audioSource;
        source.clip="audio/generated/noise.wav";
        source.maxDistance=100;
        nodes[1].node.transform->position={3,1,0};
        tick(); require(runtime.playing(0,1),"Autoplay failed");
        const auto count=audio.mixer().getVoiceCount();
        runtime.command(0,1,AudioCommand::Play); runtime.command(0,1,AudioCommand::Play); tick();
        require(audio.mixer().getVoiceCount()==count,"play created duplicate voices");
        auto right=render(audio); require(right[1]>right[0]*1.1,"Right source is not right localized");
        nodes[1].node.transform->position={-3,1,0}; tick();
        auto left=render(audio); require(left[0]>left[1]*1.1,"Moving source did not switch to left");
        nodes[0].node.transform->rotation.y=180; tick();
        right=render(audio); require(right[1]>right[0]*1.1,"Listener rotation did not rotate localization");
        nodes[0].node.transform->rotation.y=0;
        nodes[1].node.transform->position={0,1,-1}; tick();
        const auto nearEnergy=total(render(audio)); require(nearEnergy>0.01,"Silent scene sound");
        nodes[1].node.transform->position={0,1,-80}; tick();
        const auto farEnergy=total(render(audio)); std::printf("distance energy near=%g far=%g\n",nearEnergy,farEnergy);
        require(farEnergy<nearEnergy*.1,"Distance attenuation failed");
        nodes[1].node.transform->position={0,1,-1};
        nodes[0].node.audioListener->enabled=false; tick();
        const auto absent=total(render(audio)); std::printf("listener absent=%g\n",absent); require(absent<1.0e-10,"Missing listener did not mute spatial voice");
        require(runtime.playing(0,1),"Missing listener stopped voice clock");
        nodes[0].node.audioListener->enabled=true; tick();
        require(total(render(audio))>0.01,"Returning listener did not restore sound");
        // A higher priority listener facing backwards reverses localization.
        nodes[3].node.audioListener=AudioListenerComponent{true,10};
        nodes[3].node.transform=Transform{}; nodes[3].node.transform->position={0,1,0};
        nodes[3].node.transform->rotation.y=180; nodes[1].node.transform->position={3,1,0}; tick();
        left=render(audio); require(left[0]>left[1]*1.1,"Listener priority not applied");
        nodes[3].node.audioListener->priority=0; tick();
        right=render(audio); require(right[1]>right[0]*1.1,"Listener tie did not use hierarchy order");
        AudioWorld::ListenerPose editorEar; editorEar.position={0,1,0};
        nodes[3].node.audioListener->priority=INT_MAX;
        runtime.spatialStep(expanded,editorEar); runtime.step(expanded);
        right=render(audio); require(right[1]>right[0]*1.1,"Editor listener did not override maximum scene priority");
        editorEar.right={-1,0,0}; editorEar.forward={0,0,1};
        runtime.spatialStep(expanded,editorEar); runtime.step(expanded);
        left=render(audio); require(left[0]>left[1]*1.1,"Editor listener rotation did not rotate localization");
        editorEar.position={0,1,1000}; runtime.spatialStep(expanded,editorEar); runtime.step(expanded);
        require(total(render(audio))<1.0e-10,"Editor listener translation did not update attenuation");
        nodes[0].node.audioListener->enabled=false; nodes[3].node.audioListener->enabled=false;
        editorEar=AudioWorld::ListenerPose{}; editorEar.position={0,1,0};
        runtime.spatialStep(expanded,editorEar); runtime.step(expanded);
        require(total(render(audio))>.01,"Editor listener required a scene listener");
        tick(); require(total(render(audio))<1.0e-10,"Removing editor listener did not restore missing-listener mute");
        nodes[0].node.audioListener->enabled=true; nodes[3].node.audioListener->enabled=true; tick();
        left=render(audio); require(left[0]>left[1]*1.1,"Removing editor listener did not restore scene priority");
        nodes[3].node.audioListener->priority=0; tick();
        runtime.command(0,1,AudioCommand::Pause); tick();
        require(!runtime.playing(0,1) && total(render(audio))<1.0e-10,"Source pause failed");
        runtime.command(0,1,AudioCommand::Play); tick();
        require(runtime.playing(0,1) && total(render(audio))>.01,"Source resume failed");
        runtime.setPaused(true); require(total(render(audio))<1.0e-10,"Play pause failed");
        runtime.setPaused(false); require(total(render(audio))>.01,"Play resume failed");
        mixer.buses[2].mute=true; runtime.setMixer(mixer);
        require(total(render(audio))<1.0e-10,"SFX bus mute failed");
        mixer.buses[2].mute=false; mixer.buses[2].volume=.5f; runtime.setMixer(mixer);
        const auto half=total(render(audio));
        mixer.buses[2].volume=1; runtime.setMixer(mixer);
        const auto full=total(render(audio));
        require(half>full*.15 && half<full*.35,"Bus gain did not scale PCM");
        mixer.buses[2].parent=2; mixer.buses[1].mute=true; runtime.setMixer(mixer);
        require(total(render(audio))<1.0e-10,"Parent bus mute failed");
        mixer.buses[1].mute=false; mixer.buses[2].name="Effects renamed"; runtime.setMixer(mixer);
        require(total(render(audio))>.01,"Bus rename broke routing");
        source.bus=999; tick(); require(total(render(audio))>.01 && !warnings.empty(),"Missing bus fallback failed");
        source.enabled=false; tick(); require(!runtime.playing(0,1),"Disabling source did not stop it");
        source.enabled=true; source.clip="missing.wav"; tick(); runtime.command(0,1,AudioCommand::Play); tick();
        require(!runtime.playing(0,1),"Missing clip produced playback");
        source.clip="audio/generated/stereo.wav"; tick(); runtime.command(0,1,AudioCommand::Play); tick();
        require(!runtime.playing(0,1),"3D stereo was accepted");
        source.streaming=true; tick(); require(!runtime.playing(0,1),"3D streaming was accepted");
        source.spatial=false; source.loop=false; tick(); runtime.command(0,1,AudioCommand::Play); tick();
        require(total(render(audio))>.01,"2D streaming is silent");
        nodes[0].node.audioListener->enabled=false; nodes[3].node.audioListener->enabled=false; tick();
        require(total(render(audio))>.01,"Missing listener muted 2D sound");
        render(audio,1200); tick(); require(!runtime.playing(0,1),"Streaming voice did not finish");
        runtime.command(0,1,AudioCommand::Play); tick(); require(runtime.playing(0,1),"Finished source did not restart");
        runtime.command(0,1,AudioCommand::Stop); tick(); require(!runtime.playing(0,1),"Source stop failed");
        nodes[1].node.audioSource.reset(); tick(); require(audio.mixer().getVoiceCount()==5,"Removed source leaked voices");
        // Commands from a frame with no UpdateLoop remain pending until the next audio stage.
        runtime.command(0,3,AudioCommand::Play);
        runFrame(defaultStages(),true,0,[&](const auto& stage){if(stage=="SoLoud") runtime.step(expanded);});
        require(!runtime.playing(0,3),"Zero-step frame consumed audio command");
        tick(); require(runtime.playing(0,3),"Pending command lost");
        const auto result=scripts.evaluate(R"(import^ throne
for^ _^, node in^ throne.query("AudioSource") {
    if^ node.name() = "UI Sound" {
        let^ sound:throne.AudioSource|nil^ = node.component(throne.AudioSource)
        if^ sound != nil^ {
            sound.setVolume(0.25)
            sound.setLoop(true^)
            sound.setBus(4)
            sound.play()
        }
    }
}
for^ _^, node in^ throne.query("AudioListener") {
    let^ ear=node.audioListener()
    if^ ear != nil^ {
        ear.setPriority(7)
        ear.setEnabled(true^)
    }
}
)");
        require(result.text.empty(),result.text.c_str()); tick();
        require(nodes[2].node.audioSource->volume==.25f && nodes[2].node.audioSource->bus==4,"Audio script writes not applied");
        require(nodes[0].node.audioListener->priority==7 && nodes[0].node.audioListener->enabled,"Listener script writes not applied");
        {
            ScriptRuntime renderEvents(world,expanded,root.parent_path(),{},PlayScope::RenderEvents,&assets);
            const auto result=renderEvents.evaluate(R"(import^ throne
for^ _^, node in^ throne.query("AudioSource") {
    let^ sound=node.audioSource()
    if^ sound != nil^ { sound.setVolume(0) }
}
)");
            require(!result.text.empty() && nodes[2].node.audioSource->volume==.25f,"RenderEvent modified audio state");
        }
        runtime.clear(); require(audio.mixer().getVoiceCount()==0,"Play stop leaked voices");

        {
            LevelExpansion twins;
            twins.instances.push_back({1,{},{}});
            for(NodeId id:{1,2}) {
                SceneNode node; node.id=id; node.name="Shared clip";
                node.audioSource=AudioSourceComponent{};
                node.audioSource->clip="audio/generated/noise.wav";
                node.audioSource->spatial=false; node.audioSource->autoPlay=true; node.audioSource->loop=true;
                twins.instances[0].expansion.nodes.push_back({{id},Expansion::npos,node});
            }
            AudioWorld independent(audio,root,MixerDefinition{});
            independent.step(twins);
            require(independent.playing(0,0) && independent.playing(0,1),"Shared clip did not create independent voices");
            independent.command(0,0,AudioCommand::Stop); independent.step(twins);
            require(!independent.playing(0,0) && independent.playing(0,1),"Stopping one instance stopped another");
            auto added=twins.instances[0].expansion.nodes[1]; added.path={3}; added.node.id=3;
            twins.instances[0].expansion.nodes.push_back(added); independent.step(twins);
            require(independent.playing(0,2),"New runtime source did not autoplay");
        }
        require(audio.mixer().getVoiceCount()==0,"Session destruction leaked voices");

        const auto temp=fs::temp_directory_path()/"throne_audio_world_smoke";
        fs::create_directories(temp);
        mixer.save(temp/"roundtrip.mixer.lton");
        require(MixerDefinition::load(temp/"roundtrip.mixer.lton").buses[2].name=="Effects renamed","Mixer roundtrip failed");
        auto invalid=mixer; invalid.buses[1].parent=3;
        rejects([&]{invalid.validate();},"Cyclic mixer accepted");
        invalid=mixer; invalid.buses[1].id=1; rejects([&]{invalid.validate();},"Duplicate bus accepted");
        invalid=mixer; invalid.buses[1].parent=999; rejects([&]{invalid.validate();},"Missing parent accepted");
        const auto scene=world.scenes.at(world.level.scenes.front().scene); scene.save(temp/"roundtrip.scene.lton");
        const auto loaded=Scene::load(temp/"roundtrip.scene.lton");
        require(loaded.nodes[0].audioListener==scene.nodes[0].audioListener && loaded.nodes[1].audioSource==scene.nodes[1].audioSource,"Audio component roundtrip failed");
        Prefab prefab; auto& child=prefab.addNode("Audio"); child.audioSource=AudioSourceComponent{}; child.audioListener=AudioListenerComponent{};
        prefab.save(temp/"sound.prefab.lton");
        Scene instances; auto& a=instances.addNode("First"); a.prefab="sound.prefab.lton";
        a.overrides.node({}).audioSource=AudioSourceComponent{}; a.overrides.node({}).audioSource->volume=.3f;
        auto& b=instances.addNode("Second"); b.prefab="sound.prefab.lton";
        Library local(temp); const auto expandedPrefab=expand(instances,local);
        require(expandedPrefab.errors.empty() && expandedPrefab.nodes.size()==2,"Audio prefab expansion failed");
        require(expandedPrefab.nodes[0].node.audioSource->volume==.3f && expandedPrefab.nodes[1].node.audioSource->volume==1,"Audio prefab overrides leaked");
        instances.save(temp/"instances.scene.lton");
        const auto serializedInstances=expand(Scene::load(temp/"instances.scene.lton"),local);
        require(serializedInstances.nodes[0].node.audioSource->volume==.3f,"Serialized prefab audio override lost");
        auto conflict=world;
        conflict.level.systems.push_back({1,"SoLoud",{}});
        rejects([&]{ScriptRuntime::schedule(conflict,expanded);},"User System with audio stage name accepted");
        rejects([&]{MixerDefinition::load(temp/"missing.mixer.lton");},"Missing mixer accepted");
        auto legacy=world.level;
        legacy.save(temp/"legacy.level.lton");
        std::ifstream input(temp/"legacy.level.lton"); std::string text((std::istreambuf_iterator<char>(input)),{}); input.close();
        for(const auto name:{"SteamAudio","SoLoud"}) {
            const auto at=text.find(std::string("\"")+name+"\""); require(at!=std::string::npos,"Cannot prepare legacy fixture");
            const auto comma=text.find(',',at); text.erase(at,comma-at+1);
        }
        lton::writeAtomically(temp/"legacy.level.lton",text);
        require(Level::load(temp/"legacy.level.lton").stages==defaultStages(),"Legacy stages not upgraded");
        std::puts("Scene audio, mixer, scripts, Kenney assets and serialization verified"); return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"audio_world_smoke: %s\n",e.what()); return 1; }
}
