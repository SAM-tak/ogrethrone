#include "showcase_smoke.h"
#include "editor/Editor.h"
#include <OgreCamera.h>
#include <OgreForwardPlusBase.h>
#include <OgreLight.h>
#include <OgreRoot.h>
#include <OgreRenderSystem.h>
#include <OgreTextureGpuManager.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsDatablock.h>
#include <OgreHlmsUnlitDatablock.h>
#include <OgreImage2.h>
#include <OgreWindow.h>
#include <imgui_internal.h>
#include <ParticleSystem/OgreParticleSystem2.h>
#include <ParticleSystem/OgreParticleSystemManager2.h>
#include <ParticleSystem/OgreParticleAffector2.h>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <cstdio>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <OgreWindowEventUtilities.h>
#endif

namespace throne {
void closePlayingSmoke(Editor& editor, Ogre::Window& window) {
#ifdef _WIN32
    if(!editor.playing()) throw std::runtime_error("Close smoke requires an active Play session");
    void* handle=nullptr; window.getCustomAttribute("WINDOW",&handle);
    if(!PostMessageW(static_cast<HWND>(handle),WM_CLOSE,0,0)) throw std::runtime_error("Could not post WM_CLOSE");
    Ogre::WindowEventUtilities::messagePump();
    const auto before=Ogre::Root::getSingleton().getNextFrameNumber();
    if(editor.frame(1.0f/60) || !editor.shouldClose()) throw std::runtime_error("Native close did not end Play's main loop");
    if(Ogre::Root::getSingleton().getNextFrameNumber()!=before)
        throw std::runtime_error("Closing Play rendered another frame before shutdown");
    std::puts("Native close during Play accepted without another render or simulation frame");
#endif
}

void showcaseSmoke(Editor& editor, Ogre::Window& window, Ogre::SceneManager& scene, Ogre::Camera& camera,
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
    const auto checkPresentation=[&](const std::string& name) {
        // Read the actual ImGui output, not just the scene's RGB render target.
        window.setManualSwapRelease(true); frame();
        Ogre::Image2 presented; presented.convertFromTexture(window.getTexture(),0,0);
        window.performManualRelease(); window.setManualSwapRelease(false);
        const auto raw=capture(name+"-scene");
        std::istringstream header(raw); std::string magic; unsigned width,height,maximum;
        header >> magic >> width >> height >> maximum; header.get();
        const auto start=static_cast<std::size_t>(header.tellg());
        const auto* viewport=ImGui::FindWindowByName("Viewport");
        require(viewport && magic=="P6" && maximum==255,"Viewport presentation capture unavailable");
        const auto origin=viewport->DC.CursorStartPos;
        const ImGuiWindow* statistics=nullptr;
        for(const auto* child:ImGui::GetCurrentContext()->Windows)
            if(child->ParentWindow==viewport && std::string(child->Name).find("ViewportStatistics")!=std::string::npos)
                statistics=child;
        std::ofstream output(captures/(name+"-presented.ppm"),std::ios::binary);
        output << "P6\n" << width << ' ' << height << "\n255\n";
        unsigned differences=0,samples=0;
        for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
            const auto colour=presented.getColourAt(unsigned(origin.x)+x,unsigned(origin.y)+y,0);
            const unsigned char rgb[]={static_cast<unsigned char>(std::clamp(colour.r,0.0f,1.0f)*255),
                static_cast<unsigned char>(std::clamp(colour.g,0.0f,1.0f)*255),
                static_cast<unsigned char>(std::clamp(colour.b,0.0f,1.0f)*255)};
            output.write(reinterpret_cast<const char*>(rgb),3);
            // Exclude the toolbar, breadcrumb and statistics overlay.
            if(x<2 || y<48 || x+2>=width || y+2>=height) continue;
            const ImVec2 screen(origin.x+x,origin.y+y);
            if(statistics && statistics->Rect().Contains(screen)) continue;
            for(unsigned c=0;c<3;++c) {
                differences+=std::abs(int(rgb[c])-int(static_cast<unsigned char>(raw[start+(y*width+x)*3+c])))>3;
                ++samples;
            }
        }
        output.close();
        std::printf("Viewport presentation: %u / %u channel differences\n",differences,samples);
        require(samples>1000 && differences==0,"ImGui changed the composited scene colours");
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
        require(editor.viewFromFirstCamera(),"Showcase view camera missing");
        settle();
        // The showcase's nodes and the View camera node.
        require(editor.frameTimings().nodes==(forward?213u:85u),"Showcase nodes were lost while opening");
        require(editor.frameTimings().bodies==0,"A showcase unexpectedly created physics bodies");
        require(lightCount(Ogre::Light::LT_DIRECTIONAL)==1,"Showcase has duplicate or missing sunlight");
        require(lightCount(Ogre::Light::LT_POINT)==(forward?128u:0u),"Point lights did not survive level switching");
        require(lightCount(Ogre::Light::LT_SPOTLIGHT)==(area?0u:2u),"Spot lights did not survive level switching");
        require(lightCount(Ogre::Light::LT_AREA_APPROX)==(area?2u:0u),"Area lights did not survive level switching");
        require(scene.getForwardPlus()->getForwardPlusMethod()==Ogre::ForwardPlusBase::MethodForwardClustered,"Clustered renderer missing");
        const auto before=capture(name);
        require(before.size()>10000,"Showcase capture is empty");
        if(forward) {
            require(camera.getPosition().length()>100,"The view camera did not frame the large Forward3D scene");
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
    require(editor.openLevel("levels/particlefx2.level.lton"),"ParticleFX2 level failed to open");
    require(editor.viewFromFirstCamera(),"ParticleFX2 view camera missing");
    editor.setShadowsEnabled(true);
    settle();
    for(int i=0;i<150;++i) frame();
    require(editor.frameTimings().nodes==11 && editor.frameTimings().particleSystems==5 && editor.frameTimings().particles>100,
            "ParticleFX2 preview did not emit all five systems");
    auto* smoke=Ogre::Root::getSingleton().getParticleSystemManager2()->getParticleSystemDef("Particle/SmokeUnlit");
    require(smoke->getNumAffectors()==3,"Smoke inheritance lost a growth, rotation or colour affector");
    bool scaler=false;
    for(const auto* affector:smoke->getAffectors()) if(affector->getType()=="Scaler") scaler=affector->getParameter("rate")=="25";
    require(scaler,"Smoke expansion rate was not parsed");
    capture("particlefx2-preview");
    auto* ring=static_cast<Ogre::HlmsUnlitDatablock*>(Ogre::Root::getSingleton().getHlmsManager()->getDatablock("Examples/Aureola"));
    require(ring->getBlendblock()->mSourceBlendFactor==Ogre::SBF_SOURCE_ALPHA &&
            ring->getBlendblock()->mDestBlendFactor==Ogre::SBF_ONE_MINUS_SOURCE_ALPHA &&
            !ring->getMacroblock()->mDepthWrite,"Aureola must alpha-blend without writing depth");
    editor.setParticlePreview(false); settle();
    require(editor.frameTimings().particleSystems==0 && editor.frameTimings().particles==0,"Disabling particle preview left live particles");
    const auto noParticles=capture("particlefx2-disabled");
    tap(ImGuiKey_F5); require(editor.playing(),"ParticleFX2 motion script failed to start");
    for(int i=0;i<180;++i) frame();
    require(editor.frameTimings().particleSystems==5 && editor.frameTimings().particles>100,"ParticleFX2 did not run in Play");
    require(noParticles!=capture("particlefx2-play"),"ParticleFX2 did not affect the rendered viewport");
    Ogre::SceneNode* moving=nullptr;
    const auto particleState=[&] {
        std::set<const Ogre::ParticleSystemDef*> definitions;
        auto systems=scene.getMovableObjectIterator("ParticleSystem2");
        while(systems.hasMoreElements()) {
            const auto* system=static_cast<Ogre::ParticleSystem2*>(systems.getNext());
            const auto* def=system->getParticleSystemDef(); definitions.insert(def);
            if(def->getName()=="Particle/SmokeUnlit") moving=system->getParentSceneNode();
        }
        std::vector<float> state;
        for(const auto* def:definitions) {
            const auto data=def->getParticleCpuData();
            for(std::size_t i=0;i<def->getQuota();i+=ARRAY_PACKED_REALS) {
                float ttl[ARRAY_PACKED_REALS]; std::memcpy(ttl,data.mTimeToLive+i/ARRAY_PACKED_REALS,sizeof ttl);
                for(unsigned lane=0;lane<ARRAY_PACKED_REALS;++lane) if(ttl[lane]>0) {
                    Ogre::Vector3 pos; data.mPosition[i/ARRAY_PACKED_REALS].getAsVector3(pos,lane);
                    require(std::isfinite(ttl[lane]) && std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z),"Particle state is not finite");
                    state.insert(state.end(),{ttl[lane],pos.x,pos.y,pos.z});
                }
            }
        }
        return state;
    };
    tap(ImGuiKey_Pause); require(editor.paused(),"ParticleFX2 Pause failed");
    const auto frozen=particleState();
    require(frozen.size()>400 && moving && moving->getPosition().x<24,"Particle or platform simulation did not advance");
    const auto platform=moving->getPosition();
    for(int i=0;i<30;++i) frame();
    require(frozen==particleState() && moving->getPosition()==platform,"Pause advanced particle positions, lifetimes or platform motion");
    capture("particlefx2-paused");
    const char* particleNames[]={"Examples/PurpleFountain","Examples/Aureola","Particle/SmokePBS","Particle/SmokeUnlit","Particle/SpherePBS"};
    auto* particleManager=scene.getParticleSystemManager2();
    for(const auto* name:particleNames) particleManager->getParticleSystemDef(name)->setVisibilityFlags(0u);
    settle(); const auto emptyFx=capture("particlefx2-hidden");
    unsigned effectIndex=0;
    for(const auto* name:particleNames) {
        auto* def=particleManager->getParticleSystemDef(name); def->setVisibilityFlags(0xFFFFFFFFu); settle();
        const auto image=capture("particlefx2-effect-"+std::to_string(effectIndex++));
        require(emptyFx!=image,"A ParticleFX2 effect did not contribute visible pixels");
        if(std::string(name)=="Examples/Aureola") {
            checkPresentation("particlefx2-ring");
            // Transparent quad corners must preserve the background, rather than paint black sheets.
            const auto pixels=emptyFx.find('\n',emptyFx.find('\n',emptyFx.find('\n')+1)+1)+1;
            require(image.size()==emptyFx.size() && pixels>0 && pixels+3<image.size(),"Particle capture dimensions changed");
            const auto background=emptyFx.substr(pixels,3);
            for(std::size_t p=pixels;p+2<image.size();p+=3) if(emptyFx.compare(p,3,background)==0) {
                for(unsigned c=0;c<3;++c)
                    require(int(static_cast<unsigned char>(image[p+c]))+2>=int(static_cast<unsigned char>(background[c])),
                            "Aureola transparent corners darkened the background");
            }
        }
        def->setVisibilityFlags(0u);
    }
    for(const auto* name:particleNames) particleManager->getParticleSystemDef(name)->setVisibilityFlags(0xFFFFFFFFu);
    editor.setParticleBlending(0); settle(); const auto blended=capture("particlefx2-alpha-blend");
    checkPresentation("particlefx2-alpha-blend");
    for(const char* material:{"Particle/SmokePBS","Particle/SmokeUnlit","Particle/SpherePBS"})
        require(!Ogre::Root::getSingleton().getHlmsManager()->getDatablock(material)->getAlphaHashing(),"Particle alpha-blending switch failed");
    editor.setParticleBlending(1); settle();
    checkPresentation("particlefx2-alpha-hash");
    require(blended!=capture("particlefx2-alpha-hash"),"Particle blending modes produced identical images");
    require(frozen==particleState(),"Blending preview advanced a paused particle simulation");
    tap(ImGuiKey_Pause); for(int i=0;i<30;++i) frame();
    require(frozen!=particleState(),"ParticleFX2 did not resume");
    tap(ImGuiKey_F5); settle();
    require(!editor.playing() && editor.frameTimings().particleSystems==0 && editor.frameTimings().particles==0,
            "Stop left ParticleFX2 instances or live particles");
    require(moving->getPosition()==Ogre::Vector3(25,2,-50),"Stop did not restore the moving smoke platform");
    // Re-entering a scene must recreate valid instances from the retained template pools.
    editor.setParticlePreview(true);
    require(editor.openLevel("levels/area-lights.level.lton"),"Could not leave the ParticleFX2 level"); settle();
    require(editor.frameTimings().particleSystems==0,"Particle systems leaked into another level");
    require(editor.openLevel("levels/particlefx2.level.lton"),"Could not reopen the ParticleFX2 level"); settle();
    require(editor.frameTimings().particleSystems==5 && editor.frameTimings().particles>0,"ParticleFX2 level did not reopen");
    tap(ImGuiKey_F5); for(int i=0;i<180;++i) frame();
    closePlayingSmoke(editor,window);
    std::puts("Showcase smoke passed: four levels, lighting, ParticleFX2 preview/blending, frozen particle lifetimes, Play/Pause/Stop and re-entry");
}
}
