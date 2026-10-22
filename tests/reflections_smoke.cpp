#include "audio/AcousticWorld.h"
#include "audio/AudioWorld.h"
#include "audio/SpatialSound.h"
#include "script/ScriptRuntime.h"
#include <soloud_wav.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {
void require(bool yes,const char* message) { if(!yes) throw std::runtime_error(message); }
double render(throne::AudioSystem& audio,unsigned blocks,unsigned skip=0) {
    std::array<float,1024> pcm; double energy=0;
    for(unsigned b=0;b<blocks;++b) {
        audio.mixer().mix(pcm.data(),512);
        for(float v:pcm) { require(std::isfinite(v),"Nonfinite reflection audio"); if(b>=skip) energy+=double(v)*v; }
    }
    return energy;
}
throne::Scene room() {
    using namespace throne;
    Scene scene;
    for(unsigned i=0;i<6;++i) {
        auto& wall=scene.addNode("Room wall"); wall.box.reset(); wall.acousticGeometry=AcousticGeometryComponent{};
        auto& g=*wall.acousticGeometry; g.shape=AcousticShape::Box; g.absorption={.15f,.2f,.3f};
        const auto axis=i/2; g.boxSize={10,10,10}; g.boxSize.data()[axis]=.2f;
        wall.transform->position.data()[axis]=(i%2?1.0f:-1.0f)*5;
    }
    return scene;
}
}
int main() {
    using namespace throne; namespace fs=std::filesystem;
    try {
        AudioSystem audio(true); std::vector<std::string> errors;
        const auto root=fs::temp_directory_path()/"throne_reflections_smoke"; fs::create_directories(root);
        World world; world.scenes["room.scene.lton"]=room(); world.level.addScene("room.scene.lton");
        Library library(root); auto expanded=expand(world,library);
        std::vector<std::vector<AcousticWorld::Pose>> poses(1);
        for(const auto& node:expanded.instances[0].expansion.nodes) {
            AcousticWorld::Pose pose; pose.position=node.node.transform->position; poses[0].push_back(pose);
        }
        AcousticWorld::Pose ear; ear.position={1,0,0};
        AcousticWorld::Source source{{0,6},{},false,"original"}; source.pose.position={-1,0,0};
        std::shared_ptr<const ReflectionResult> reflective,absorbing;
        {
            AcousticWorld geometry(audio,root,[&](const auto& e){errors.push_back(e);});
            geometry.syncGeometry(expanded,poses);
            const auto wait=[&] {
                const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);
                while(std::chrono::steady_clock::now()<until) {
                    const auto results=geometry.reflections(ear,{source});
                    if(const auto found=results.find(source.key);found!=results.end()) return found->second;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                throw std::runtime_error("Asynchronous reflection result timed out");
            };
            reflective=wait(); require(reflective->parameters.ir!=nullptr,"Missing native reflection IR");
            source.identity="replacement";
            require(geometry.reflections(ear,{source}).empty(),"IR from replaced source was reused");
            source.identity="original";
            for(auto& node:expanded.instances[0].expansion.nodes) node.node.acousticGeometry->absorption={1,1,1};
            geometry.syncGeometry(expanded,poses);
            require(geometry.reflections(ear,{source}).empty(),"Old material IR survived a geometry revision");
            absorbing=wait();
            require(geometry.reflections(ear,{}).empty(),"Listener/source removal retained reflection outputs");
            // Destroy while another task may be pending. Completed IR owners must survive.
        }
        auto pulse=std::make_shared<SoLoud::Wav>();
        std::array<float,882> samples; std::uint32_t random=1;
        for(auto& sample:samples) { random=random*1664525u+1013904223u; sample=(float(random>>16)/32768-1)*.15f; }
        require(pulse->loadRawWave(samples.data(),unsigned(samples.size()),44100,1,true)==SoLoud::SO_NO_ERROR,"Cannot load reflection pulse");
        const IPLCoordinateSpace3 orientation{{1,0,0},{0,1,0},{0,0,-1},{0,0,0}};
        const auto energy=[&](std::shared_ptr<const ReflectionResult> result,float gain) {
            IPLDirectEffectParams direct{}; direct.flags=IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION; direct.occlusion=0;
            SpatialSound sound(audio,pulse,{-1,0,0},direct);
            sound.updateReflections(std::move(result),gain,orientation);
            const auto voice=audio.mixer().play(sound);
            const auto late=render(audio,110,10); // Entirely after the 20ms source has ended.
            require(!audio.mixer().isValidVoiceHandle(voice),"Reflection tail did not finish");
            return late;
        };
        const auto wet=energy(reflective,1), dry=energy(reflective,0), absorbed=energy(absorbing,1);
        require(wet>1.0e-8,"No reflected/reverberant tail after source end");
        require(dry<1.0e-12,"Reflection volume zero leaked wet sound");
        require(absorbed<wet*.1,"Absorption did not reduce reverberation");

        // Publish/retire native IRs while a real audio reader is inside getAudio.
        // Zero direct sound isolates the wet path, including orientation updates.
        {
            IPLDirectEffectParams direct{}; direct.flags=IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION;
            SpatialSound sound(audio,pulse,{-1,0,0},direct); sound.setLooping(true);
            sound.updateReflections(reflective,1,orientation); audio.mixer().play(sound);
            std::atomic<bool> finite{true};
            std::thread reader([&] {
                std::array<float,1024> pcm;
                for(unsigned i=0;i<160;++i) {
                    audio.mixer().mix(pcm.data(),512);
                    for(float v:pcm) if(!std::isfinite(v)) finite=false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
            for(unsigned i=0;i<160;++i) {
                auto turned=orientation; if(i%2) turned.right={0,0,-1},turned.ahead={-1,0,0};
                sound.updateReflections(i%3==0?nullptr:(i%3==1?reflective:absorbing),float(i%2),turned);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            reader.join(); require(finite,"Concurrent IR updates produced invalid PCM");
            sound.stop();
        }

        // Full scene integration: opaque wall removes the direct path but room returns sound.
        auto& scene=world.scenes.at("room.scene.lton");
        auto& listener=scene.addNode("Listener"); listener.box.reset(); listener.audioListener=AudioListenerComponent{};
        listener.transform->position={1,0,0};
        auto& emitter=scene.addNode("Emitter"); emitter.box.reset(); emitter.transform->position={-1,0,0};
        emitter.audioSource=AudioSourceComponent{}; emitter.audioSource->clip="audio/generated/noise.wav";
        emitter.audioSource->loop=true; emitter.audioSource->autoPlay=true; emitter.audioSource->transmission=false;
        emitter.audioSource->reflections=true; emitter.audioSource->bus=3;
        auto& divider=scene.addNode("Divider"); divider.box.reset(); divider.acousticGeometry=AcousticGeometryComponent{};
        divider.acousticGeometry->shape=AcousticShape::Box; divider.acousticGeometry->boxSize={.1f,8,8};
        Library sampleLibrary(std::filesystem::path(THRONE_AUDIO_SAMPLE)/"assets"); auto live=expand(world,sampleLibrary);
        AudioWorld runtime(audio,std::filesystem::path(THRONE_AUDIO_SAMPLE)/"assets",MixerDefinition::defaults(),[&](const auto& e){errors.push_back(e);});
        const auto tick=[&]{runtime.spatialStep(live); runtime.step(live);};
        tick(); const auto baseline=render(audio,20);
        require(baseline<1.0e-9,"Opaque divider leaked direct sound before reflection result");
        double sceneWet=0; const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(sceneWet<1.0e-7 && std::chrono::steady_clock::now()<until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); tick(); sceneWet=render(audio,24,12);
        }
        require(sceneWet>1.0e-7,"Scene reflection result did not reach PCM");
        runtime.setPaused(true); require(render(audio,20,12)<1.0e-12,"Pause did not silence reflection path");
        runtime.setPaused(false); require(render(audio,24,12)>1.0e-7,"Reflection voice did not resume");
        auto mixer=MixerDefinition::defaults(); for(auto& bus:mixer.buses) if(bus.id==3) bus.mute=true;
        runtime.setMixer(mixer); require(render(audio,24,12)<1.0e-12,"Bus mute did not silence reflection path");
        for(auto& bus:mixer.buses) bus.mute=false; runtime.setMixer(mixer);
        ScriptRuntime scripts(world,live,root,{},PlayScope::Level);
        auto edit=scripts.evaluate(R"(import^ throne
for^ _^, node in^ throne.query("AudioSource") {
    let^ source=node.audioSource()
    if^ source != nil^ {
        source.setReflections(false^)
        source.setReflectionVolume(0.25)
    }
}
)");
        require(edit.text.empty(),edit.text.c_str()); tick();
        require(render(audio,24,12)<1.0e-12,"Script reflection disable did not silence wet path");
        live.instances[0].expansion.nodes[7].node.audioSource->reflections=true;
        live.instances[0].expansion.nodes[6].node.audioListener->enabled=false; tick();
        require(render(audio,24,12)<1.0e-12,"Missing listener leaked wet sound");
        runtime.clear(); require(audio.mixer().getVoiceCount()==0,"Reflection session leaked voices");
        scene.save(root/"room.scene.lton"); const auto loaded=Scene::load(root/"room.scene.lton");
        require(loaded.nodes[7].audioSource==scene.nodes[7].audioSource,"Reflection settings lost in scene serialization");
        require(errors.empty(),"Unexpected reflection diagnostic");
        std::printf("Reflections passed: late energy %.8g, absorbed %.8g, scene wet %.8g\n",wet,absorbed,sceneWet);
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"reflections_smoke: %s\n",e.what()); return 1; }
}
