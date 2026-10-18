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
        for(const char* name:{"forward3d","pbs-materials","area-lights"}) {
            const bool forward=std::string(name)=="forward3d",area=std::string(name)=="area-lights";
            auto level=throne::Level::load(project.resolve(std::string("levels/")+name+".level.lton"));
            require(level.scenes.size()==1 && !level.rendering.defaultSun && level.rendering.openingCamera,"Showcase environment missing");
            auto scene=throne::Scene::load(project.resolve(level.scenes[0].scene));
            require(scene.nodes.size()==(forward?212u:84u),"Unexpected showcase node count");
            unsigned palette=0,animated=0,point=0,spot=0,rect=0;
            for(const auto& n:scene.nodes) {
                if(n.name.rfind("Palette",0)==0) {
                    ++palette;
                    require(n.material && n.sphere && !n.material->reflectionMap.empty(),"Palette lost PBS/reflection data");
                    require(n.material->roughness>=.02f && n.material->roughness<=1,"Palette roughness invalid");
                }
                animated+=!n.behaviours.empty();
                if(n.light) {
                    point+=n.light->type==throne::LightType::Point;
                    spot+=n.light->type==throne::LightType::Spot;
                    rect+=n.light->type==throne::LightType::AreaApprox;
                }
            }
            require(palette==(forward?0u:64u) && animated==16,"Showcase palette or animation missing");
            require(point==(forward?128u:0u) && spot==(area?0u:2u) && rect==(area?2u:0u),"Showcase light types missing");
            scene.save(temporary/"scene.lton"); const auto restored=throne::Scene::load(temporary/"scene.lton");
            restored.save(temporary/"scene-again.lton");
            const auto bytes=[](const auto& path) { std::ifstream f(path,std::ios::binary); return std::string(std::istreambuf_iterator<char>(f),{}); };
            require(bytes(temporary/"scene.lton")==bytes(temporary/"scene-again.lton"),"Light/material round trip is unstable");
            level.save(temporary/"level.lton"); const auto reopened=throne::Level::load(temporary/"level.lton");
            require(reopened.rendering.maxDistance==level.rendering.maxDistance && reopened.rendering.distance==level.rendering.distance &&
                reopened.rendering.ambientUpper.y==level.rendering.ambientUpper.y,"Level rendering settings lost on save");
        }
        throne::Prefab prefab;
        auto& root=prefab.addNode("Emitter"); root.light.emplace(); root.material.emplace(); const auto id=root.id;
        prefab.save(temporary/"light.prefab.lton");
        throne::Scene holder;
        auto& instance=holder.addNode("Instance"); instance.prefab="light.prefab.lton";
        auto& layer=instance.overrides.node({id}); layer.light.emplace(); layer.light->width=7; layer.light->type=throne::LightType::AreaLtc;
        layer.material.emplace(); layer.material->metalness=.75f;
        holder.save(temporary/"holder.lton");
        throne::Library library(temporary);
        const auto expanded=throne::expand(throne::Scene::load(temporary/"holder.lton"),library);
        require(expanded.errors.empty() && expanded.nodes.size()==1 && expanded.nodes[0].node.light->width==7 &&
            expanded.nodes[0].node.material->metalness==.75f,"Prefab light/material override lost");
        for(const char* invalid:{"light = {type = \"Unknown\"}","light = {direction = {0,0,0}}", "light = {innerAngle=60,outerAngle=30}",
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
