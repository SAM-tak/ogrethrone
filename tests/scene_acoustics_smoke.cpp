#include "audio/AcousticWorld.h"
#include "audio/AudioWorld.h"
#include "script/ScriptRuntime.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace {
void require(bool yes,const char* message) { if(!yes) throw std::runtime_error(message); }
template<class F> void rejects(F call,const char* message) {
    bool failed=false; try { call(); } catch(const std::exception&) { failed=true; } require(failed,message);
}
void signal(const std::filesystem::path& path) {
    std::ofstream out(path,std::ios::binary);
    const auto u16=[&](unsigned v) { out.put(char(v)); out.put(char(v>>8)); };
    const auto u32=[&](unsigned v) { u16(v&65535); u16(v>>16); };
    out.write("RIFF",4); u32(36+48000*2); out.write("WAVEfmt ",8); u32(16); u16(1); u16(1);
    u32(48000); u32(96000); u16(2); u16(16); out.write("data",4); u32(48000*2);
    for(unsigned i=0;i<48000;++i) {
        constexpr double pi=3.141592653589793;
        const auto value=int(1500*(std::sin(2*pi*250*i/48000)+std::sin(2*pi*8000*i/48000)));
        u16(static_cast<unsigned>(value)&65535);
    }
}
struct Spectrum { double energy=0,low=0,high=0; };
Spectrum render(throne::AudioSystem& audio) {
    std::array<float,1024> pcm;
    double re[2]{},im[2]{}; Spectrum out;
    constexpr double pi=3.141592653589793;
    for(unsigned block=0;block<80;++block) {
        audio.mixer().mix(pcm.data(),512);
        if(block<40) continue; // Drain EQ, HRTF and nested-bus buffering after updates.
        for(unsigned i=0;i<512;++i) {
            const auto v=double(pcm[2*i]+pcm[2*i+1])*.5;
            require(std::isfinite(v),"Nonfinite acoustic PCM"); out.energy+=v*v;
            for(unsigned b=0;b<2;++b) {
                const auto phase=2*pi*(b?8000:250)*(block*512+i)/48000;
                re[b]+=v*std::cos(phase); im[b]+=v*std::sin(phase);
            }
        }
    }
    out.low=re[0]*re[0]+im[0]*im[0]; out.high=re[1]*re[1]+im[1]*im[1]; return out;
}
}
int main() {
    using namespace throne;
    namespace fs=std::filesystem;
    try {
        AudioSystem audio(true);
        const auto root=fs::temp_directory_path()/"throne_scene_acoustics_smoke";
        fs::create_directories(root); signal(root/"bands.wav");
        std::vector<std::string> errors;
        LevelExpansion expanded; expanded.instances.push_back({1,{},{}});
        SceneNode wall; wall.id=1; wall.name="Wall"; wall.acousticGeometry=AcousticGeometryComponent{};
        wall.acousticGeometry->shape=AcousticShape::Box; wall.acousticGeometry->boxSize={.2f,6,6};
        expanded.instances[0].expansion.nodes.push_back({{1},Expansion::npos,wall});
        auto& nodes=expanded.instances[0].expansion.nodes;
        AcousticWorld geometry(audio,root,[&](const auto& e){errors.push_back(e);});
        std::vector<std::vector<AcousticWorld::Pose>> poses{{AcousticWorld::Pose{}}};
        AcousticWorld::Pose ear; ear.position={3,.5f,0};
        AcousticWorld::Source source{{0,0},{},true}; source.pose.position={-3,.5f,0};
        const auto direct=[&] {
            geometry.syncGeometry(expanded,poses);
            return geometry.simulate(ear,{source}).at(source.key);
        };
        auto result=direct(); require(result.occlusion==0,"Box proxy did not occlude source");
        require(result.transmission.x>result.transmission.z && result.transmission.x<1,"Frequency-dependent transmission not simulated");
        poses[0][0].position.z=10; require(direct().occlusion==1,"Moving acoustic geometry did not clear occlusion");
        poses[0][0].position.z=0; require(direct().occlusion==0,"Returning acoustic geometry did not restore occlusion");
        nodes[0].node.acousticGeometry->transmission={.8f,.4f,.2f};
        require(direct().transmission.x>result.transmission.x,"Material change did not update simulation");
        nodes[0].node.acousticGeometry->enabled=false; require(direct().occlusion==1,"Disabling geometry did not remove occlusion");
        nodes[0].node.acousticGeometry->enabled=true;
        nodes[0].node.acousticGeometry->shape=AcousticShape::Render;
        nodes[0].node.plane=PlanePrimitive{}; nodes[0].node.plane->width=6; nodes[0].node.plane->depth=6;
        poses[0][0].axes={{{0,1,0},{-1,0,0},{0,0,1}}};
        require(direct().occlusion==0,"Rotated render plane did not occlude");
        poses[0][0].axes={{{1,0,0},{0,1,0},{0,0,1}}};
        require(direct().occlusion==1,"Plane rotation did not update acoustic transform");
        nodes[0].node.plane.reset(); nodes[0].node.sphere=SpherePrimitive{};
        ear.position.y=source.pose.position.y=0;
        require(direct().occlusion==0,"Sphere render geometry did not occlude");
        poses[0][0].scale={.1f,.1f,.1f}; ear.position.y=source.pose.position.y=.2f;
        require(direct().occlusion==1,"Acoustic scale was ignored");
        poses[0][0]=AcousticWorld::Pose{}; nodes[0].node.sphere.reset(); nodes[0].node.capsule=CapsulePrimitive{};
        require(direct().occlusion==0,"Capsule render geometry did not occlude");
        nodes[0].node.capsule.reset(); nodes[0].node.box=BoxPrimitive{};
        require(direct().occlusion==0,"Box render geometry did not occlude");
        nodes[0].node.acousticGeometry.reset(); require(direct().occlusion==1,"Deleting acoustic component left stale geometry");
        require(errors.empty(),"Unexpected acoustic geometry diagnostic");
        // FBX meshes are parsed in the same metres/right-handed axes as the renderer.
        nodes[0].node.box.reset(); nodes[0].node.acousticGeometry=AcousticGeometryComponent{};
        nodes[0].node.model=ModelComponent{}; nodes[0].node.model->path="blender_279_default_7400_binary.fbx";
        {
            AcousticWorld model(audio,THRONE_UFBX_DATA,[&](const auto& e){errors.push_back(e);});
            model.syncGeometry(expanded,poses);
            require(model.simulate(ear,{source}).at(source.key).occlusion==0,"FBX acoustic geometry did not occlude");
            require(errors.empty(),"FBX acoustic geometry failed to load");
        }

        World world; auto& scene=world.scenes["scene.lton"];
        auto& listener=scene.addNode("Listener"); listener.box.reset(); listener.audioListener=AudioListenerComponent{};
        auto& emitter=scene.addNode("Emitter"); emitter.box.reset(); emitter.transform->position={0,0,-4};
        emitter.audioSource=AudioSourceComponent{}; emitter.audioSource->clip="bands.wav";
        emitter.audioSource->autoPlay=true; emitter.audioSource->loop=true; emitter.audioSource->minDistance=10;
        auto& authoredWall=scene.addNode("Wall"); authoredWall.box.reset(); authoredWall.transform->position={0,0,-2};
        authoredWall.acousticGeometry=AcousticGeometryComponent{};
        authoredWall.acousticGeometry->shape=AcousticShape::Box; authoredWall.acousticGeometry->boxSize={6,6,.1f};
        authoredWall.acousticGeometry->transmission={.5f,.1f,.005f}; authoredWall.acousticGeometry->enabled=false;
        world.level.addScene("scene.lton"); Library library(root); auto playing=expand(world,library);
        AudioWorld runtime(audio,root,MixerDefinition{},[&](const auto& e){errors.push_back(e);});
        const auto tick=[&]{runtime.spatialStep(playing); runtime.step(playing);};
        auto& live=playing.instances[0].expansion.nodes;
        tick(); const auto clear=render(audio); require(clear.energy>.01 && clear.low>1 && clear.high>1,"Silent baseline bands");
        live[2].node.acousticGeometry->enabled=true; live[1].node.audioSource->transmission=false;
        tick(); const auto blocked=render(audio); require(blocked.energy<clear.energy*1.0e-5,"Occlusion result was not applied to PCM");
        live[1].node.audioSource->transmission=true; tick(); const auto filtered=render(audio);
        require(filtered.energy>1.0e-8 && filtered.energy<clear.energy*.5,"Wall transmission level incorrect");
        require(filtered.high/filtered.low < clear.high/clear.low*.2,"Wall did not attenuate high frequencies more strongly");
        live[1].node.audioSource->occlusion=false; tick();
        require(render(audio).energy>clear.energy*.8,"Per-source occlusion bypass failed");
        live[1].node.audioSource->occlusion=true; live[2].node.acousticGeometry->transmission={1,1,1}; tick();
        require(render(audio).energy>clear.energy*.8,"Live material change did not restore direct sound");
        live[2].node.transform->position.x=10; tick(); require(render(audio).energy>clear.energy*.8,"Moving wall broke direct sound");
        live[2].node.transform->position.x=0; live[2].node.acousticGeometry->transmission={0,0,0}; tick();
        require(render(audio).energy<clear.energy*1.0e-5,"Opaque material leaked direct sound");
        live[2].node.acousticGeometry->enabled=false; tick(); require(render(audio).energy>clear.energy*.8,"Geometry removal did not restore PCM");
        ScriptRuntime scripts(world,playing,root,{},PlayScope::Level);
        const auto edit=scripts.evaluate(R"(import^ throne
for^ _^, node in^ throne.query("AcousticGeometry") {
    let^ geometry:throne.AcousticGeometry|nil^=node.component(throne.AcousticGeometry)
    if^ geometry != nil^ {
        geometry.setEnabled(true^)
        geometry.setTransmission(throne.vec3(0.4,0.2,0.01))
        geometry.setAbsorption(throne.vec3(0.1,0.2,0.3))
        geometry.setScattering(0.6)
    }
}
for^ _^, node in^ throne.query("AudioSource") {
    let^ sound=node.audioSource()
    if^ sound != nil^ {
        sound.setOcclusion(true^)
        sound.setTransmission(false^)
    }
}
)");
        require(edit.text.empty(),edit.text.c_str()); tick();
        require(render(audio).energy<clear.energy*1.0e-5,"Acoustic script edits not applied");
        runFrame(defaultStages(),false,3,[&](const auto& stage){
            if(stage=="SteamAudio") runtime.spatialStep(playing);
            if(stage=="SoLoud") runtime.step(playing);
            if(stage=="OGRE") render(audio);
        });
        require(runtime.playing(0,1),"Parallel acoustic simulation stopped voice");
        runtime.clear(); require(audio.mixer().getVoiceCount()==0,"Acoustic session leaked audio voices");
        scene.save(root/"scene.lton"); auto loaded=Scene::load(root/"scene.lton");
        require(loaded.nodes[2].acousticGeometry==scene.nodes[2].acousticGeometry,"Acoustic geometry serialization failed");
        auto invalid=*loaded.nodes[2].acousticGeometry; invalid.transmission.x=2;
        rejects([&]{invalid.validate();},"Invalid acoustic coefficient accepted");
        Prefab prefab; auto& node=prefab.addNode("Sound wall"); node.acousticGeometry=AcousticGeometryComponent{};
        prefab.save(root/"wall.prefab.lton"); Scene placed; auto& instance=placed.addNode("Placed wall"); instance.prefab="wall.prefab.lton";
        instance.overrides.node({}).acousticGeometry=AcousticGeometryComponent{};
        instance.overrides.node({}).acousticGeometry->transmission={.8f,.7f,.6f}; placed.save(root/"placed.scene.lton");
        Library assets(root); const auto resolved=expand(Scene::load(root/"placed.scene.lton"),assets);
        require(resolved.nodes[0].node.acousticGeometry->transmission.x==.8f,"Acoustic prefab override lost");
        require(errors.empty(),"Unexpected audio acoustic diagnostic");
        std::printf("Scene acoustics passed: clear %.8g, blocked %.8g, transmitted %.8g, band ratios %.8g -> %.8g\n",
            clear.energy,blocked.energy,filtered.energy,clear.high/clear.low,filtered.high/filtered.low);
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"scene_acoustics_smoke: %s\n",e.what()); return 1; }
}
