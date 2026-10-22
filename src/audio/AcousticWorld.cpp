#include "audio/AcousticWorld.h"
#include "editor/PrimitiveGeometry.h"
#include "runtime/TaskPool.h"
#include <ufbx.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <set>

namespace throne {
namespace {
void check(IPLerror error) { if(error!=IPL_STATUS_SUCCESS) throw std::runtime_error("Steam Audio scene/simulator creation failed"); }
IPLVector3 vector(Vec3 v) { return {v.x,v.y,v.z}; }
IPLCoordinateSpace3 coordinates(const AcousticWorld::Pose& pose) {
    return {vector(pose.axes[0]),vector(pose.axes[1]),vector(pose.axes[2]*-1),vector(pose.position)};
}
IPLMatrix4x4 matrix(const AcousticWorld::Pose& pose) {
    IPLMatrix4x4 out{};
    for(unsigned i=0;i<3;++i) {
        const auto axis=pose.axes[i]*pose.scale.data()[i];
        for(unsigned j=0;j<3;++j) out.elements[j][i]=axis.data()[j];
        out.elements[i][3]=pose.position.data()[i];
    }
    out.elements[3][3]=1; return out;
}
bool sameMatrix(const IPLMatrix4x4& a,const IPLMatrix4x4& b) {
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) if(a.elements[r][c]!=b.elements[r][c]) return false;
    return true;
}
struct SceneLease {
    IPLScene scene=nullptr;
    explicit SceneLease(IPLScene value):scene(iplSceneRetain(value)) {}
    ~SceneLease() { iplSceneRelease(&scene); }
};
struct ReflectionGeometry { std::shared_ptr<SceneLease> mesh; IPLMatrix4x4 transform; };
struct ReflectionBatch {
    IPLContext context=nullptr;
    IPLScene scene=nullptr;
    IPLSimulator simulator=nullptr;
    std::vector<IPLInstancedMesh> meshes;
    std::vector<IPLSource> sources;
    std::vector<ReflectionGeometry> geometry;
    ~ReflectionBatch() {
        for(auto& source:sources) iplSourceRelease(&source);
        iplSimulatorRelease(&simulator);
        for(auto& mesh:meshes) iplInstancedMeshRelease(&mesh);
        iplSceneRelease(&scene); iplContextRelease(&context);
    }
};
using ReflectionMap=std::map<AcousticWorld::Key,std::shared_ptr<const ReflectionResult>>;
ReflectionMap simulateReflections(IPLContext context,const AcousticWorld::Pose& listener,
                                 const std::vector<AcousticWorld::Source>& sources,
                                 std::vector<ReflectionGeometry> geometry) {
    // Every batch is immutable once complete. Neither scene commits nor a later simulation
    // can mutate the IR currently used by the audio thread.
    auto batch=std::make_shared<ReflectionBatch>(); batch->context=iplContextRetain(context);
    batch->geometry=std::move(geometry);
    IPLSceneSettings scene{}; scene.type=IPL_SCENETYPE_DEFAULT;
    check(iplSceneCreate(context,&scene,&batch->scene));
    for(const auto& g:batch->geometry) {
        IPLInstancedMeshSettings settings{g.mesh->scene,g.transform};
        batch->meshes.push_back(nullptr);
        check(iplInstancedMeshCreate(batch->scene,&settings,&batch->meshes.back()));
        iplInstancedMeshAdd(batch->meshes.back(),batch->scene);
    }
    iplSceneCommit(batch->scene);
    IPLSimulationSettings settings{}; settings.flags=IPL_SIMULATIONFLAGS_REFLECTIONS;
    settings.sceneType=IPL_SCENETYPE_DEFAULT; settings.reflectionType=IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    settings.maxNumRays=4096; settings.numDiffuseSamples=32; settings.maxDuration=1; settings.maxOrder=reflectionOrder;
    settings.maxNumSources=reflectionSourceLimit; settings.numThreads=1;
    settings.samplingRate=reflectionRate; settings.frameSize=reflectionBlock;
    check(iplSimulatorCreate(context,&settings,&batch->simulator));
    iplSimulatorSetScene(batch->simulator,batch->scene);
    for(const auto& source:sources) {
        IPLSourceSettings settings{IPL_SIMULATIONFLAGS_REFLECTIONS}; batch->sources.push_back(nullptr);
        check(iplSourceCreate(batch->simulator,&settings,&batch->sources.back()));
        iplSourceAdd(batch->sources.back(),batch->simulator);
        IPLSimulationInputs inputs{}; inputs.flags=IPL_SIMULATIONFLAGS_REFLECTIONS;
        inputs.source=coordinates(source.pose); inputs.reverbScale[0]=inputs.reverbScale[1]=inputs.reverbScale[2]=1;
        iplSourceSetInputs(batch->sources.back(),IPL_SIMULATIONFLAGS_REFLECTIONS,&inputs);
    }
    iplSimulatorCommit(batch->simulator);
    IPLSimulationSharedInputs shared{}; shared.listener=coordinates(listener);
    shared.numRays=4096; shared.numBounces=16; shared.duration=1; shared.order=reflectionOrder; shared.irradianceMinDistance=1;
    iplSimulatorSetSharedInputs(batch->simulator,IPL_SIMULATIONFLAGS_REFLECTIONS,&shared);
    iplSimulatorRunReflections(batch->simulator);
    ReflectionMap out;
    for(std::size_t i=0;i<sources.size();++i) {
        IPLSimulationOutputs outputs{}; iplSourceGetOutputs(batch->sources[i],IPL_SIMULATIONFLAGS_REFLECTIONS,&outputs);
        auto result=std::make_shared<ReflectionResult>(); result->owner=batch; result->parameters=outputs.reflections;
        result->parameters.type=IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
        result->parameters.numChannels=reflectionChannels; result->parameters.irSize=reflectionSamples;
        out.emplace(sources[i].key,std::move(result));
    }
    return out;
}
struct Mesh {
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;
    void triangle(IPLVector3 a,IPLVector3 b,IPLVector3 c) {
        if(vertices.size()>INT_MAX-3) throw std::runtime_error("Acoustic mesh is too large");
        for(auto v:{a,b,c}) if(!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
            throw std::runtime_error("Nonfinite acoustic mesh vertex");
        const Vec3 u{b.x-a.x,b.y-a.y,b.z-a.z},v{c.x-a.x,c.y-a.y,c.z-a.z};
        const Vec3 cross{u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
        const auto area=cross.x*cross.x+cross.y*cross.y+cross.z*cross.z;
        if(!std::isfinite(area)) throw std::runtime_error("Acoustic triangle exceeds numeric range");
        if(area==0) return;
        const auto index=static_cast<int>(vertices.size());
        vertices.insert(vertices.end(),{a,b,c}); triangles.push_back({{index,index+1,index+2}});
    }
    void append(const Mesh& mesh) {
        for(const auto& t:mesh.triangles) triangle(mesh.vertices[t.indices[0]],mesh.vertices[t.indices[1]],mesh.vertices[t.indices[2]]);
    }
};
Mesh modelMesh(const std::filesystem::path& path) {
    if(path.extension()==".mesh") throw std::runtime_error("Use an acoustic Box proxy for OGRE .mesh models: "+path.u8string());
    ufbx_load_opts options{}; options.target_axes=ufbx_axes_right_handed_y_up; options.target_unit_meters=1;
    ufbx_error error{};
    std::unique_ptr<ufbx_scene,decltype(&ufbx_free_scene)> scene(ufbx_load_file(path.u8string().c_str(),&options,&error),ufbx_free_scene);
    if(!scene) throw std::runtime_error("Cannot read acoustic model: "+path.u8string());
    Mesh out;
    for(const auto* node:scene->nodes) if(const auto* mesh=node->mesh) {
        std::vector<std::uint32_t> indices(mesh->max_face_triangles*3);
        for(const auto face:mesh->faces) {
            const auto count=ufbx_triangulate_face(indices.data(),indices.size(),mesh,face);
            for(std::size_t i=0;i<count;++i) {
                IPLVector3 points[3];
                for(unsigned c=0;c<3;++c) {
                    const auto local=ufbx_get_vertex_vec3(&mesh->vertex_position,indices[i*3+c]);
                    const auto p=ufbx_transform_position(&node->geometry_to_world,local);
                    points[c]={static_cast<float>(p.x),static_cast<float>(p.y),static_cast<float>(p.z)};
                }
                out.triangle(points[0],points[1],points[2]);
            }
        }
    }
    if(out.triangles.empty()) throw std::runtime_error("Acoustic model contains no triangles: "+path.u8string());
    return out;
}
}
struct AcousticWorld::State {
    struct Geometry {
        AcousticGeometryComponent config;
        PrimitiveShape primitive;
        std::string model;
        std::filesystem::file_time_type stamp{};
        IPLScene subScene=nullptr;
        IPLStaticMesh mesh=nullptr;
        IPLInstancedMesh instance=nullptr;
        IPLMatrix4x4 transform{};
        ~Geometry() { iplInstancedMeshRelease(&instance); iplStaticMeshRelease(&mesh); iplSceneRelease(&subScene); }
    };
    struct Source {
        IPLSource source=nullptr;
        ~Source() { iplSourceRelease(&source); }
    };
    struct Asset { std::filesystem::file_time_type stamp{}; Mesh mesh; std::string error; };
    AudioSystem& audio;
    std::filesystem::path root;
    Log log;
    IPLScene scene=nullptr;
    IPLSimulator simulator=nullptr;
    std::map<std::pair<std::uint32_t,NodePath>,std::unique_ptr<Geometry>> geometry;
    std::map<Key,std::unique_ptr<Source>> sources;
    std::map<std::string,Asset> models;
    struct Job {
        ReflectionMap results;
        std::vector<AcousticWorld::Source> inputs;
        std::uint64_t revision=0;
    };
    std::uint64_t revision=0, resultRevision=0;
    ReflectionMap reflectionResults;
    std::map<Key,std::string> identities;
    std::shared_ptr<Job> job;
    std::chrono::steady_clock::time_point nextReflection{};
    std::future<void> reflectionFuture;
    // Last member, destroyed first; destructor explicitly joins before native handles.
    std::unique_ptr<TaskPool> reflectionWorker;
    const AssetDatabase* assets=nullptr;
    State(AudioSystem& audio,std::filesystem::path root,Log log):audio(audio),root(std::move(root)),log(std::move(log)) {}
    ~State() {
        reflectionWorker.reset(); job.reset(); reflectionResults.clear();
        sources.clear(); iplSimulatorRelease(&simulator);
        geometry.clear(); iplSceneRelease(&scene);
    }
    const Mesh& model(const std::string& path,std::filesystem::file_time_type stamp) {
        auto [it,added]=models.try_emplace(path); auto& asset=it->second;
        if(added || asset.stamp!=stamp) {
            asset.stamp=stamp; asset.error.clear(); asset.mesh={};
            try { asset.mesh=modelMesh(root/std::filesystem::u8path(path)); }
            catch(const std::exception& e) { asset.error=e.what(); }
        }
        if(!asset.error.empty()) throw std::runtime_error(asset.error);
        return asset.mesh;
    }
    void create(Geometry& g) {
        Mesh data;
        if(g.config.shape==AcousticShape::Box || g.primitive.kind!=PrimitiveKind::None) {
            auto shape=g.primitive;
            if(g.config.shape==AcousticShape::Box) shape.kind=PrimitiveKind::Box;
            const auto primitive=primitiveMesh(shape);
            const auto point=[&](std::uint32_t i) {
                auto v=primitive.vertices.at(i).position;
                if(g.config.shape==AcousticShape::Box) v={v.x*g.config.boxSize.x,v.y*g.config.boxSize.y,v.z*g.config.boxSize.z};
                return vector(v);
            };
            for(std::size_t i=0;i<primitive.indices.size();i+=3)
                data.triangle(point(primitive.indices[i]),point(primitive.indices[i+1]),point(primitive.indices[i+2]));
        }
        if(g.config.shape==AcousticShape::Render && !g.model.empty()) data.append(model(g.model,g.stamp));
        if(data.triangles.empty()) throw std::runtime_error("AcousticGeometry needs a render primitive/model or Box proxy");
        IPLSceneSettings sceneSettings{}; sceneSettings.type=IPL_SCENETYPE_DEFAULT;
        check(iplSceneCreate(audio.steam(),&sceneSettings,&g.subScene));
        const auto& c=g.config;
        IPLMaterial material{{c.absorption.x,c.absorption.y,c.absorption.z},c.scattering,{c.transmission.x,c.transmission.y,c.transmission.z}};
        std::vector<int> materials(data.triangles.size(),0);
        IPLStaticMeshSettings settings{static_cast<int>(data.vertices.size()),static_cast<int>(data.triangles.size()),1,
            data.vertices.data(),data.triangles.data(),materials.data(),&material};
        check(iplStaticMeshCreate(g.subScene,&settings,&g.mesh));
        iplStaticMeshAdd(g.mesh,g.subScene); iplSceneCommit(g.subScene);
        IPLInstancedMeshSettings instance{g.subScene,g.transform};
        check(iplInstancedMeshCreate(scene,&instance,&g.instance)); iplInstancedMeshAdd(g.instance,scene);
    }
};
AcousticWorld::AcousticWorld(AudioSystem& audio,std::filesystem::path root,Log log,const AssetDatabase* assets)
    :state_(std::make_unique<State>(audio,std::move(root),std::move(log))) {
    auto& s=*state_;
    s.assets=assets;
    IPLSceneSettings scene{}; scene.type=IPL_SCENETYPE_DEFAULT; check(iplSceneCreate(audio.steam(),&scene,&s.scene));
    IPLSimulationSettings settings{}; settings.flags=IPL_SIMULATIONFLAGS_DIRECT; settings.sceneType=IPL_SCENETYPE_DEFAULT;
    settings.maxNumOcclusionSamples=1; settings.maxNumSources=1024; settings.numThreads=1;
    settings.samplingRate=48000; settings.frameSize=512;
    check(iplSimulatorCreate(audio.steam(),&settings,&s.simulator));
    iplSimulatorSetScene(s.simulator,s.scene); iplSimulatorCommit(s.simulator);
}
AcousticWorld::~AcousticWorld()=default;
void AcousticWorld::syncGeometry(const LevelExpansion& world,const std::vector<std::vector<Pose>>& poses) {
    auto& s=*state_; bool changed=false;
    std::set<std::pair<std::uint32_t,NodePath>> seen;
    for(std::size_t i=0;i<world.instances.size();++i) {
        const auto& instance=world.instances[i];
        for(std::size_t n=0;n<instance.expansion.nodes.size();++n) {
            const auto& node=instance.expansion.nodes[n]; const auto& c=node.node.acousticGeometry;
            if(!c || !c->enabled) continue;
            c->validate();
            const auto key=std::make_pair(instance.id,node.path); seen.insert(key);
            auto& entry=s.geometry[key];
            const auto shape=c->shape==AcousticShape::Render?primitiveShape(node.node):PrimitiveShape{};
            const auto* ref=c->shape==AcousticShape::Render && node.node.model ? &node.node.model->path : nullptr;
            const auto model=!ref ? std::string{} : s.assets ? s.assets->path(*ref) : ref->path;
            std::error_code error;
            const auto stamp=model.empty()?std::filesystem::file_time_type{}:std::filesystem::last_write_time(s.root/std::filesystem::u8path(model),error);
            const auto transform=matrix(poses.at(i).at(n));
            if(!entry || !(entry->config==*c) || entry->primitive!=shape || entry->model!=model || entry->stamp!=stamp) {
                if(entry && entry->instance) iplInstancedMeshRemove(entry->instance,s.scene);
                entry=std::make_unique<State::Geometry>(); entry->config=*c; entry->primitive=shape;
                entry->model=model; entry->stamp=stamp; entry->transform=transform; changed=true;
                try { s.create(*entry); }
                catch(const std::exception& e) { if(s.log) s.log("AcousticGeometry "+node.node.name+": "+e.what()); }
            } else if(entry->instance && !sameMatrix(entry->transform,transform)) {
                iplInstancedMeshUpdateTransform(entry->instance,s.scene,transform); entry->transform=transform; changed=true;
            }
        }
    }
    for(auto it=s.geometry.begin();it!=s.geometry.end();) {
        if(!seen.count(it->first)) {
            if(it->second->instance) iplInstancedMeshRemove(it->second->instance,s.scene);
            it=s.geometry.erase(it); changed=true;
        } else ++it;
    }
    if(changed) { iplSceneCommit(s.scene); ++s.revision; }
}
std::map<AcousticWorld::Key,std::shared_ptr<const ReflectionResult>> AcousticWorld::reflections(const Pose& listener,const std::vector<Source>& requested) {
    auto& s=*state_;
    if(s.reflectionFuture.valid() && s.reflectionFuture.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try {
            s.reflectionFuture.get();
            s.reflectionResults=std::move(s.job->results); s.resultRevision=s.job->revision;
            s.identities.clear(); for(const auto& source:s.job->inputs) s.identities.emplace(source.key,source.identity);
        } catch(const std::exception& e) { if(s.log) s.log(std::string("Steam Audio reflections: ")+e.what()); }
        s.job.reset();
    }
    ReflectionMap available;
    if(s.resultRevision==s.revision) for(const auto& source:requested) {
        const auto found=s.reflectionResults.find(source.key), end=s.reflectionResults.end();
        const auto identity=s.identities.find(source.key);
        if(found!=end && identity!=s.identities.end() && identity->second==source.identity) available.emplace(*found);
    }
    const bool hasGeometry=std::any_of(s.geometry.begin(),s.geometry.end(),[](const auto& g){return g.second->instance!=nullptr;});
    if(requested.empty() || !hasGeometry) { s.reflectionResults.clear(); s.identities.clear(); return {}; }
    const auto now=std::chrono::steady_clock::now();
    if(!s.reflectionFuture.valid() && now>=s.nextReflection) {
        if(!s.reflectionWorker) s.reflectionWorker=std::make_unique<TaskPool>(1);
        auto job=std::make_shared<State::Job>(); job->revision=s.revision;
        job->inputs.assign(requested.begin(),requested.begin()+(std::min)(requested.size(),std::size_t(reflectionSourceLimit)));
        if(requested.size()>reflectionSourceLimit && s.log) s.log("Steam Audio reflections: budget is 16 sources; remaining sources use direct sound only");
        std::vector<ReflectionGeometry> geometry;
        for(const auto& [key,g]:s.geometry) if(g->instance)
            geometry.push_back({std::make_shared<SceneLease>(g->subScene),g->transform});
        s.job=job;
        s.reflectionFuture=s.reflectionWorker->submit([context=s.audio.steam(),listener,job,geometry=std::move(geometry)]() mutable {
            job->results=simulateReflections(context,listener,job->inputs,std::move(geometry));
        });
        s.nextReflection=now+std::chrono::milliseconds(100);
    }
    return available;
}
std::map<AcousticWorld::Key,AcousticWorld::Direct> AcousticWorld::simulate(const Pose& listener,const std::vector<AcousticWorld::Source>& inputs) {
    auto& s=*state_; std::set<Key> seen; bool changed=false;
    for(const auto& input:inputs) {
        seen.insert(input.key); auto& entry=s.sources[input.key];
        if(!entry) {
            entry=std::make_unique<State::Source>(); IPLSourceSettings settings{IPL_SIMULATIONFLAGS_DIRECT};
            check(iplSourceCreate(s.simulator,&settings,&entry->source)); iplSourceAdd(entry->source,s.simulator); changed=true;
        }
        IPLSimulationInputs settings{}; settings.flags=IPL_SIMULATIONFLAGS_DIRECT;
        settings.directFlags=static_cast<IPLDirectSimulationFlags>(IPL_DIRECTSIMULATIONFLAGS_OCCLUSION |
            (input.transmission?IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION:0));
        settings.source=coordinates(input.pose); settings.occlusionType=IPL_OCCLUSIONTYPE_RAYCAST;
        settings.numOcclusionSamples=1; settings.numTransmissionRays=16;
        iplSourceSetInputs(entry->source,IPL_SIMULATIONFLAGS_DIRECT,&settings);
    }
    for(auto it=s.sources.begin();it!=s.sources.end();) {
        if(!seen.count(it->first)) { iplSourceRemove(it->second->source,s.simulator); it=s.sources.erase(it); changed=true; }
        else ++it;
    }
    if(changed) iplSimulatorCommit(s.simulator);
    IPLSimulationSharedInputs shared{}; shared.listener=coordinates(listener);
    iplSimulatorSetSharedInputs(s.simulator,IPL_SIMULATIONFLAGS_DIRECT,&shared);
    if(!inputs.empty()) iplSimulatorRunDirect(s.simulator);
    std::map<Key,Direct> results;
    for(const auto& input:inputs) {
        IPLSimulationOutputs out{}; iplSourceGetOutputs(s.sources.at(input.key)->source,IPL_SIMULATIONFLAGS_DIRECT,&out);
        results[input.key]={out.direct.occlusion,{out.direct.transmission[0],out.direct.transmission[1],out.direct.transmission[2]}};
    }
    return results;
}
}
