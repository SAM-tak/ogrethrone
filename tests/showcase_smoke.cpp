#include "showcase_smoke.h"
#include "editor/Editor.h"
#include <OgreCamera.h>
#include <OgreForwardPlusBase.h>
#include <OgreLight.h>
#include <OgreRoot.h>
#include <OgreRenderSystem.h>
#include <OgreTextureGpuManager.h>
#include <OgreSceneManager.h>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <cstdio>

namespace throne {
void showcaseSmoke(Editor& editor, Ogre::SceneManager& scene, Ogre::Camera& camera,
                   const std::filesystem::path& captures) {
    const auto require=[](bool ok,const char* message) { if(!ok) throw std::runtime_error(message); };
    const auto frame=[&] { require(editor.frame(1.0f/60),"Showcase frame failed"); };
    const auto settle=[&] {
        frame(); Ogre::Root::getSingleton().getRenderSystem()->getTextureGpuManager()->waitForStreamingCompletion();
        for(int i=0;i<12;++i) frame();
    };
    const auto tap=[&](ImGuiKey key) {
        editor.frame(1.0f/60,[&] { ImGui::GetIO().AddKeyEvent(key,true); });
        editor.frame(1.0f/60,[&] { ImGui::GetIO().AddKeyEvent(key,false); });
    };
    // Cascade fitting shifts edge samples after camera/scene changes. Disable it for exact pose comparisons.
    editor.setShadowsEnabled(false);
    std::filesystem::create_directories(captures);
    const auto capture=[&](const std::string& name) {
        const auto file=captures/(name+".ppm"); editor.captureViewport(file);
        std::ifstream stream(file,std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream),{});
    };
    const auto same=[](const std::string& a,const std::string& b) {
        if(a.size()!=b.size()) return false;
        // Allow a handful of GPU rounding differences, never visible movement.
        unsigned differences=0;
        for(std::size_t i=0;i<a.size();++i) if(a[i]!=b[i]) {
            if(std::abs(int(static_cast<unsigned char>(a[i]))-int(static_cast<unsigned char>(b[i])))>2 || ++differences>64) return false;
        }
        return true;
    };
    const auto lightCount=[&](Ogre::Light::LightTypes type) {
        unsigned count=0; auto lights=scene.getMovableObjectIterator("Light");
        while(lights.hasMoreElements()) {
            const auto* light=static_cast<Ogre::Light*>(lights.getNext());
            count+=light->getVisible() && light->getType()==type;
        }
        return count;
    };
    for(const char* name:{"forward3d","pbs-materials","area-lights"}) {
        const bool forward=std::string(name)=="forward3d",area=std::string(name)=="area-lights";
        require(editor.openLevel(std::string("levels/")+name+".level.lton"),"Showcase level failed to open");
        settle();
        require(editor.frameTimings().nodes==(forward?212u:84u),"Showcase nodes were lost while opening");
        require(editor.frameTimings().bodies==0,"A showcase unexpectedly created physics bodies");
        require(lightCount(Ogre::Light::LT_DIRECTIONAL)==1,"Showcase has duplicate or missing sunlight");
        require(lightCount(Ogre::Light::LT_POINT)==(forward?128u:0u),"Point lights did not survive level switching");
        require(lightCount(Ogre::Light::LT_SPOTLIGHT)==(area?0u:2u),"Spot lights did not survive level switching");
        require(lightCount(Ogre::Light::LT_AREA_APPROX)==(area?2u:0u),"Area lights did not survive level switching");
        require(scene.getForwardPlus()->getForwardPlusMethod()==Ogre::ForwardPlusBase::MethodForwardClustered,"Clustered renderer missing");
        const auto before=capture(name);
        require(before.size()>10000,"Showcase capture is empty");
        if(forward) {
            require(camera.getPosition().length()>100,"Opening camera did not frame the large Forward3D scene");
            editor.setClusterDebug(true); settle();
            require(before!=capture("forward3d-clusters"),"Cluster debug did not change the image");
            editor.setClusterDebug(false); editor.setForwardClustered(false); settle();
            require(scene.getForwardPlus()->getForwardPlusMethod()==Ogre::ForwardPlusBase::MethodForward3D,"Forward3D selection failed");
            capture("forward3d-alternative"); editor.setForwardClustered(true); settle();
        }
        if(area) {
            editor.setAreaLightModel(1); settle();
            require(lightCount(Ogre::Light::LT_AREA_LTC)==2 && lightCount(Ogre::Light::LT_AREA_APPROX)==0,"LTC preview did not switch both lights");
            require(before!=capture("area-lights-ltc"),"LTC and textured approximation rendered identically");
            editor.setAreaLightModel(-1); settle();
            require(same(before,capture("area-lights-restored")),"Authored area lights were not restored");
        }
        tap(ImGuiKey_F5); require(editor.playing(),"Showcase Play failed");
        settle(); require(before!=capture(std::string(name)+"-play"),"Showcase rotation script did not animate");
        tap(ImGuiKey_Pause); require(editor.paused(),"Showcase Pause failed");
        const auto paused=capture(std::string(name)+"-paused"); settle();
        require(editor.frameTimings().scriptMs==0 && editor.frameTimings().physicsStepMs==0,"Paused showcase still executed simulation");
        require(same(paused,capture(std::string(name)+"-paused-again")),"Paused showcase kept animating");
        tap(ImGuiKey_F5); require(!editor.playing(),"Showcase Stop failed"); settle();
        require(same(before,capture(std::string(name)+"-stopped")),"Stop did not restore the authored showcase");
    }
    std::puts("Showcase smoke passed: three levels, lights, textures, Forward3D/Clustered, LTC/Approx, animation, Pause and Stop");
}
}
