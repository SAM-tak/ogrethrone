#include "audio/AudioWorld.h"
#include "audio/AcousticWorld.h"
#include "audio/SpatialSound.h"
#include "audio/BufferedStream.h"
#include <soloud_wav.h>
#include <soloud_bus.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace throne {
namespace {
using Key=std::pair<std::size_t,std::size_t>;
float dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 product(Vec3 a,Vec3 b) { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
using Pose=AcousticWorld::Pose;
Pose compose(const Pose& parent,const Transform& local) {
    const auto rotate=[&](Vec3 v) {
        constexpr float r=.017453292519943f;
        const auto e=local.rotation*r;
        v={v.x,std::cos(e.x)*v.y-std::sin(e.x)*v.z,std::sin(e.x)*v.y+std::cos(e.x)*v.z};
        v={std::cos(e.y)*v.x+std::sin(e.y)*v.z,v.y,-std::sin(e.y)*v.x+std::cos(e.y)*v.z};
        v={std::cos(e.z)*v.x-std::sin(e.z)*v.y,std::sin(e.z)*v.x+std::cos(e.z)*v.y,v.z};
        return parent.rotate(v);
    };
    return {parent.position+parent.rotate(product(parent.scale,local.position)), product(parent.scale,local.scale),
            {{rotate({1,0,0}),rotate({0,1,0}),rotate({0,0,1})}}};
}
std::vector<Pose> poses(const LevelExpansion::Instance& instance) {
    std::vector<Pose> out;
    const auto base=compose({},instance.transform);
    for(const auto& n:instance.expansion.nodes) {
        const auto parent=n.parent==Expansion::npos?base:out.at(n.parent);
        out.push_back(n.node.transform?compose(parent,*n.node.transform):parent);
    }
    return out;
}
bool sameMixer(const MixerDefinition& a,const MixerDefinition& b) {
    if(a.buses.size()!=b.buses.size()) return false;
    for(std::size_t i=0;i<a.buses.size();++i) {
        const auto& x=a.buses[i]; const auto& y=b.buses[i];
        if(std::tie(x.id,x.parent,x.name,x.volume,x.mute)!=std::tie(y.id,y.parent,y.name,y.volume,y.mute)) return false;
    }
    return true;
}
}

struct AudioWorld::State {
    struct Slot {
        std::uint32_t scene=0;
        NodePath path;
        AudioSourceComponent config;
        std::shared_ptr<SoLoud::AudioSource> source;
        SoLoud::handle voice=0;
        bool paused=false, seen=false, playing=false;
        std::uint32_t bus=0;
        float outputVolume=1;
    };
    struct MixerInstance {
        struct Bus { std::unique_ptr<SoLoud::Bus> source; SoLoud::handle voice=0; };
        std::map<std::uint32_t,Bus> buses;
        MixerDefinition definition;
    } mixer;
    AudioSystem& audio;
    std::filesystem::path root;
    Log log;
    std::map<Key,Slot> slots;
    struct Spatial {
        Vec3 direction{0,0,-1}; IPLDirectEffectParams direct{};
        std::shared_ptr<const ReflectionResult> reflections;
        IPLCoordinateSpace3 listener{{1,0,0},{0,1,0},{0,0,-1},{0,0,0}};
    };
    std::map<Key,Spatial> spatial;
    std::unique_ptr<AcousticWorld> acoustics;
    std::map<std::string,std::shared_ptr<SoLoud::Wav>> clips;
    std::vector<std::pair<Key,AudioCommand>> commands;
    std::set<std::string> warned;
    bool paused=false;
    const AssetDatabase* assets;
    State(AudioSystem& audio,std::filesystem::path root,Log log,const AssetDatabase* assets)
        :audio(audio),root(std::move(root)),log(std::move(log)),assets(assets) {
        acoustics=std::make_unique<AcousticWorld>(audio,this->root,[this](const auto& text){warn(text);},assets);
    }
    void warn(const std::string& message) { if(warned.insert(message).second && log) log(message); }
    SoLoud::Soloud& soloud() { return audio.mixer(); }
    void stop(Slot& slot) { soloud().stop(slot.voice); slot.voice=0; slot.paused=false; slot.playing=false; }
    std::shared_ptr<SoLoud::Wav> clip(const std::string& path) {
        auto found=clips.find(path); if(found!=clips.end()) return found->second;
        auto wave=std::make_shared<SoLoud::Wav>();
        if(wave->load((root/std::filesystem::u8path(path)).u8string().c_str())!=SoLoud::SO_NO_ERROR || !wave->mSampleCount || wave->mChannels<1 || wave->mChannels>2)
            throw std::runtime_error("Cannot decode audio: "+path);
        clips.emplace(path,wave); return wave;
    }
    /// The path of the clip @p ref names, in the assets folder.
    std::string path(const AssetRef& ref) const { return assets ? assets->path(ref) : ref.path; }
    void prepare(Slot& slot) {
        const auto& c=slot.config; c.validate();
        if(c.clip.empty()) return;
        const auto name=path(c.clip);
        if(name.empty()) throw std::runtime_error("Missing audio clip asset "+std::to_string(c.clip.id));
        if(c.spatial && c.streaming) throw std::runtime_error("3D streaming is not supported: "+name);
        if(c.streaming) {
            slot.source=std::make_shared<BufferedStream>((root/std::filesystem::u8path(name)).u8string());
        } else {
            auto wave=clip(name);
            if(c.spatial) slot.source=std::make_shared<SpatialSound>(audio,wave,Vec3{0,0,-1});
            else slot.source=wave;
        }
    }
    std::uint32_t bus(std::uint32_t id) {
        if(mixer.buses.count(id)) return id;
        warn("Missing mixer bus "+std::to_string(id)+"; using Master"); return 1;
    }
    void play(Slot& slot) {
        if(!slot.source || !slot.config.enabled) return;
        if(soloud().isValidVoiceHandle(slot.voice)) {
            slot.paused=false; soloud().setPause(slot.voice,paused); return;
        }
        if(slot.config.streaming) static_cast<BufferedStream&>(*slot.source).restart();
        slot.bus=bus(slot.config.bus);
        slot.voice=mixer.buses.at(slot.bus).source->play(*slot.source,slot.outputVolume,0,true);
        if(!soloud().isValidVoiceHandle(slot.voice)) throw std::runtime_error("Audio voice limit reached");
        soloud().setLooping(slot.voice,slot.config.loop);
        soloud().setInaudibleBehavior(slot.voice,true,false);
        slot.paused=false; soloud().setPause(slot.voice,paused);
    }
    void safePlay(Slot& slot) {
        try { play(slot); }
        catch(const std::exception& e) { stop(slot); slot.source.reset(); warn(path(slot.config.clip)+": "+e.what()); }
    }
};

AudioWorld::AudioWorld(AudioSystem& audio,std::filesystem::path root,const MixerDefinition& mixer,Log log,
                       const AssetDatabase* assets)
    :state_(std::make_unique<State>(audio,std::move(root),std::move(log),assets)) { setMixer(mixer); }
AudioWorld::~AudioWorld() { clear(); }
void AudioWorld::setMixer(const MixerDefinition& definition) {
    definition.validate(); auto& s=*state_; auto& mixer=s.mixer;
    if(!mixer.buses.empty() && sameMixer(mixer.definition,definition)) return;
    bool sameTopology=mixer.buses.size()==definition.buses.size();
    for(const auto& bus:definition.buses) {
        const auto previous=std::find_if(mixer.definition.buses.begin(),mixer.definition.buses.end(),
            [&](const auto& b){return b.id==bus.id && b.parent==bus.parent;});
        if(previous==mixer.definition.buses.end() || !mixer.buses.count(bus.id)) sameTopology=false;
    }
    if(sameTopology) {
        for(const auto& bus:definition.buses) s.soloud().setVolume(mixer.buses.at(bus.id).voice,bus.mute?0:bus.volume);
        mixer.definition=definition; return;
    }
    for(const auto& b:definition.buses) if(!mixer.buses.count(b.id)) {
        auto& bus=mixer.buses[b.id]; bus.source=std::make_unique<SoLoud::Bus>();
        bus.voice=s.soloud().play(*bus.source,1,0,false);
        if(!s.soloud().isValidVoiceHandle(bus.voice)) throw std::runtime_error("Cannot create mixer bus");
        s.soloud().setProtectVoice(bus.voice,true); s.soloud().setInaudibleBehavior(bus.voice,true,false);
    }
    // Flatten before reparenting so changing a valid tree cannot form a temporary cycle.
    for(auto& [id,bus]:mixer.buses) if(id!=1) mixer.buses.at(1).source->annexSound(bus.voice);
    std::set<std::uint32_t> keep;
    for(const auto& b:definition.buses) keep.insert(b.id);
    for(auto& [key,slot]:s.slots) {
        const auto id=keep.count(slot.config.bus)?slot.config.bus:1;
        if(s.soloud().isValidVoiceHandle(slot.voice)) mixer.buses.at(id).source->annexSound(slot.voice);
        slot.bus=id;
    }
    for(auto it=mixer.buses.begin();it!=mixer.buses.end();) {
        if(!keep.count(it->first)) { s.soloud().stop(it->second.voice); it=mixer.buses.erase(it); }
        else ++it;
    }
    for(const auto& b:definition.buses) {
        auto& bus=mixer.buses.at(b.id);
        if(b.id!=1) mixer.buses.at(b.parent).source->annexSound(bus.voice);
        s.soloud().setVolume(bus.voice,b.mute?0:b.volume);
    }
    mixer.definition=definition;
}
void AudioWorld::spatialStep(const LevelExpansion& world,std::optional<ListenerPose> listenerOverride) {
    auto& s=*state_; s.spatial.clear();
    if(!s.acoustics) s.acoustics=std::make_unique<AcousticWorld>(s.audio,s.root,[&s](const auto& text){s.warn(text);},s.assets);
    std::vector<std::vector<Pose>> all;
    const AudioListenerComponent* selected=nullptr; Pose ear;
    for(const auto& instance:world.instances) {
        all.push_back(poses(instance));
        for(const auto n:instance.expansion.hierarchy()) {
            const auto& c=instance.expansion.nodes[n].node.audioListener;
            if(c && c->enabled && (!selected || c->priority>selected->priority)) selected=&*c,ear=all.back()[n];
        }
    }
    const bool listener=selected || listenerOverride.has_value();
    if(listenerOverride) {
        const auto& pose=*listenerOverride;
        ear.position=pose.position; ear.axes={{pose.right,pose.up,pose.forward*-1}};
    }
    s.acoustics->syncGeometry(world,all);
    std::vector<AcousticWorld::Source> inputs;
    std::vector<AcousticWorld::Source> reflected;
    if(listener) for(std::size_t i=0;i<world.instances.size();++i) {
        const auto& nodes=world.instances[i].expansion.nodes;
        for(std::size_t n=0;n<nodes.size();++n) {
            const auto& c=nodes[n].node.audioSource;
            if(c && c->enabled && c->spatial && c->reflections && !c->clip.empty() && !c->streaming) {
                std::string identity=std::to_string(world.instances[i].id)+":"+s.path(c->clip);
                for(auto id:nodes[n].path) identity+=":"+std::to_string(id);
                reflected.push_back({{i,n},all[i][n],false,std::move(identity)});
            }
            if(c && c->enabled && c->spatial && c->occlusion && !c->clip.empty()) {
                const auto d=all[i][n].position-ear.position;
                if(dot(d,d)>1.0e-12f) inputs.push_back({{i,n},all[i][n],c->transmission});
            }
        }
    }
    const auto results=s.acoustics->simulate(ear,inputs);
    const auto reflections=s.acoustics->reflections(ear,reflected);
    for(std::size_t i=0;i<world.instances.size();++i) {
        const auto& nodes=world.instances[i].expansion.nodes;
        for(std::size_t n=0;n<nodes.size();++n) if(const auto& c=nodes[n].node.audioSource; c && c->spatial) {
            c->validate();
            const auto d=all[i][n].position-ear.position;
            const auto distance=std::sqrt(dot(d,d));
            const auto gain=listener?std::clamp((c->maxDistance-distance)/(c->maxDistance-c->minDistance),0.0f,1.0f):0.0f;
            const Vec3 direction{dot(d,ear.axes[0]),dot(d,ear.axes[1]),dot(d,ear.axes[2])};
            IPLDirectEffectParams direct{};
            direct.flags=IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION; direct.distanceAttenuation=gain;
            direct.occlusion=1; direct.transmission[0]=direct.transmission[1]=direct.transmission[2]=1;
            if(const auto at=results.find({i,n});at!=results.end()) {
                direct.flags=static_cast<IPLDirectEffectFlags>(direct.flags | IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION |
                    (c->transmission?IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION:0));
                direct.occlusion=at->second.occlusion;
                for(unsigned b=0;b<3;++b) direct.transmission[b]=at->second.transmission.data()[b];
                direct.transmissionType=IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
            }
            auto& spatial=s.spatial[{i,n}]; spatial.direction=direction; spatial.direct=direct;
            const auto vector=[](Vec3 v){return IPLVector3{v.x,v.y,v.z};};
            spatial.listener={vector(ear.axes[0]),vector(ear.axes[1]),vector(ear.axes[2]*-1),vector(ear.position)};
            if(const auto found=reflections.find({i,n});found!=reflections.end()) spatial.reflections=found->second;
        }
    }
}
void AudioWorld::step(const LevelExpansion& world) {
    auto& s=*state_;
    for(auto& [key,slot]:s.slots) slot.seen=false;
    for(std::size_t i=0;i<world.instances.size();++i) {
        const auto& instance=world.instances[i];
        for(std::size_t n=0;n<instance.expansion.nodes.size();++n) {
            const auto& node=instance.expansion.nodes[n];
            if(!node.node.audioSource) continue;
            const auto& c=*node.node.audioSource;
            auto [it,created]=s.slots.try_emplace({i,n}); auto& slot=it->second;
            const bool replaced=!created && (slot.scene!=instance.id || slot.path!=node.path);
            const bool reload=created || replaced || slot.config.clip!=c.clip || slot.config.spatial!=c.spatial || slot.config.streaming!=c.streaming;
            if(reload) {
                s.stop(slot); slot.source.reset(); slot.scene=instance.id; slot.path=node.path; slot.config=c;
                try { s.prepare(slot); } catch(const std::exception& e) { s.warn(e.what()); }
            }
            slot.config=c; slot.seen=true;
            if(!c.enabled) s.stop(slot);
            slot.outputVolume=c.volume;
            if(c.spatial && slot.source) {
                const auto at=s.spatial.find({i,n});
                const auto parameters=at==s.spatial.end()?State::Spatial{}:at->second;
                static_cast<SpatialSound&>(*slot.source).update(parameters.direction,parameters.direct);
                static_cast<SpatialSound&>(*slot.source).updateReflections(parameters.reflections,
                    c.reflections?c.reflectionVolume:0,parameters.listener);
                if(parameters.direct.distanceAttenuation==0) slot.outputVolume=0;
            }
            if(c.streaming && slot.source && static_cast<BufferedStream&>(*slot.source).failed()) {
                s.stop(slot); slot.source.reset(); s.warn("Streaming decode failed: "+s.path(c.clip));
            }
            if((created || replaced) && c.autoPlay) s.safePlay(slot);
            if(s.soloud().isValidVoiceHandle(slot.voice)) {
                s.soloud().setVolume(slot.voice,slot.outputVolume); s.soloud().setLooping(slot.voice,c.loop);
                const auto bus=s.bus(c.bus);
                if(bus!=slot.bus) s.mixer.buses.at(bus).source->annexSound(slot.voice),slot.bus=bus;
            }
        }
    }
    for(auto it=s.slots.begin();it!=s.slots.end();) {
        if(!it->second.seen) { s.stop(it->second); it=s.slots.erase(it); } else ++it;
    }
    for(const auto& [key,command]:s.commands) if(const auto it=s.slots.find(key);it!=s.slots.end()) {
        auto& slot=it->second;
        if(command==AudioCommand::Play) s.safePlay(slot);
        else if(command==AudioCommand::Stop) s.stop(slot);
        else { slot.paused=true; s.soloud().setPause(slot.voice,true); }
    }
    s.commands.clear();
    for(auto& [key,slot]:s.slots) slot.playing=s.soloud().isValidVoiceHandle(slot.voice) && !slot.paused && !s.paused;
}
void AudioWorld::command(std::size_t instance,std::size_t node,AudioCommand command) { state_->commands.push_back({{instance,node},command}); }
bool AudioWorld::playing(std::size_t instance,std::size_t node) const {
    const auto at=state_->slots.find({instance,node}); return at!=state_->slots.end() && at->second.playing;
}
void AudioWorld::setPaused(bool paused) {
    auto& s=*state_; s.paused=paused;
    for(auto& [key,slot]:s.slots) {
        s.soloud().setPause(slot.voice,paused || slot.paused);
        slot.playing=s.soloud().isValidVoiceHandle(slot.voice) && !paused && !slot.paused;
    }
}
void AudioWorld::clear() {
    auto& s=*state_;
    for(auto& [key,slot]:s.slots) s.stop(slot);
    s.slots.clear(); s.commands.clear(); s.spatial.clear(); s.clips.clear();
    for(auto& [id,bus]:s.mixer.buses) s.soloud().stop(bus.voice);
    s.mixer.buses.clear();
    s.acoustics.reset();
}
}
