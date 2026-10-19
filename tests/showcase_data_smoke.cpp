#include "editor/Expansion.h"
#include "editor/Project.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>

/// Validates authored showcase data, persistence and prefab light/material overrides without OGRE.
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("Expected showcase project path");
        const auto project=throne::Project::open(argv[1]);
        const auto temporary=std::filesystem::temp_directory_path()/"throne-showcase-data";
        std::filesystem::create_directories(temporary);
        const auto require=[](bool ok,const char* message) { if(!ok) throw std::runtime_error(message); };
        for(const char* name:{"forward3d","pbs-materials","area-lights","particlefx2"}) {
            const bool forward=std::string(name)=="forward3d",area=std::string(name)=="area-lights",particles=std::string(name)=="particlefx2";
            auto level=throne::Level::load(project.resolve(std::string("levels/")+name+".level.lton"));
            require(level.scenes.size()==1 && !level.rendering.defaultSun && level.rendering.openingCamera,"Showcase environment missing");
            auto scene=throne::Scene::load(project.resolve(level.scenes[0].scene));
            require(scene.nodes.size()==(particles?10u:forward?212u:84u),"Unexpected showcase node count");
            unsigned palette=0,animated=0,point=0,spot=0,rect=0,systems=0;
            for(const auto& n:scene.nodes) {
                if(n.name.rfind("Palette",0)==0) {
                    ++palette;
                    require(n.material && n.sphere && !n.material->reflectionMap.empty(),"Palette lost PBS/reflection data");
                    require(n.material->roughness>=.02f && n.material->roughness<=1,"Palette roughness invalid");
                }
                animated+=!n.behaviour.empty(); systems+=bool(n.particles);
                if(n.light) {
                    point+=n.light->type==throne::LightType::Point;
                    spot+=n.light->type==throne::LightType::Spot;
                    rect+=n.light->type==throne::LightType::AreaApprox;
                }
            }
            require(palette==((forward || particles)?0u:64u) && animated==(particles?2u:16u) && systems==(particles?5u:0u),"Showcase palette or animation missing");
            require(point==(forward?128u:0u) && spot==((area || particles)?0u:2u) && rect==(area?2u:0u),"Showcase light types missing");
            scene.save(temporary/"scene.lton"); const auto restored=throne::Scene::load(temporary/"scene.lton");
            restored.save(temporary/"scene-again.lton");
            const auto bytes=[](const auto& path) { std::ifstream f(path,std::ios::binary); return std::string(std::istreambuf_iterator<char>(f),{}); };
            require(bytes(temporary/"scene.lton")==bytes(temporary/"scene-again.lton"),"Light/material round trip is unstable");
            level.save(temporary/"level.lton"); const auto reopened=throne::Level::load(temporary/"level.lton");
            require(reopened.rendering.maxDistance==level.rendering.maxDistance && reopened.rendering.distance==level.rendering.distance &&
                reopened.rendering.ambientUpper.y==level.rendering.ambientUpper.y,"Level rendering settings lost on save");
        }
        throne::Prefab prefab;
        auto& root=prefab.addNode("Emitter"); root.light.emplace(); root.material.emplace(); root.particles.emplace(); const auto id=root.id;
        prefab.save(temporary/"light.prefab.lton");
        throne::Scene holder;
        auto& instance=holder.addNode("Instance"); instance.prefab="light.prefab.lton";
        auto& layer=instance.overrides.node({id}); layer.light.emplace(); layer.light->width=7; layer.light->type=throne::LightType::AreaLtc;
        layer.material.emplace(); layer.material->metalness=.75f;
        layer.particles.emplace(); layer.particles->definition="Particle/SpherePBS"; layer.particles->enabled=false;
        holder.save(temporary/"holder.lton");
        throne::Library library(temporary);
        const auto expanded=throne::expand(throne::Scene::load(temporary/"holder.lton"),library);
        require(expanded.errors.empty() && expanded.nodes.size()==1 && expanded.nodes[0].node.light->width==7 &&
            expanded.nodes[0].node.material->metalness==.75f && !expanded.nodes[0].node.particles->enabled &&
            expanded.nodes[0].node.particles->definition=="Particle/SpherePBS","Prefab light/material override lost");
        for(const char* invalid:{"light = {type = \"Unknown\"}","light = {direction = {0,0,0}}", "light = {innerAngle=60,outerAngle=30}",
            "particles = {definition = \"\"}","particles = {script = \"../escape.particle2\"}","particles = {enabled = 123}",
            "material = {roughness = -1}","material = {reflectionMap = \"../escape.dds\"}"}) {
            std::ofstream(temporary/"bad.lton")<<"format=\"ogrethrone-scene\",version=2,nodes={{id=1,name=\"Bad\",parent=0,components={"<<invalid<<"}}},";
            bool rejected=false; try { throne::Scene::load(temporary/"bad.lton"); } catch(const std::exception&) { rejected=true; }
            require(rejected,"Malformed light/material accepted");
        }
        std::filesystem::remove_all(temporary);
        std::puts("Showcase data passed: all levels, PBS palette, lights, persistence, prefab overrides and validation");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
