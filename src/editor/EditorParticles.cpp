#include "editor/Editor.h"
#include <OgreCamera.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsDatablock.h>
#include <OgreHlmsUnlitDatablock.h>
#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <ParticleSystem/OgreParticleSystem2.h>
#include <ParticleSystem/OgreParticleSystemManager2.h>

namespace throne {
void Editor::loadParticleResources(const std::string& script) {
    auto& resources=Ogre::ResourceGroupManager::getSingleton();
    if(!particleResourcesLoaded_) {
        const Ogre::String group="ThroneParticles";
        resources.createResourceGroup(group);
        resources.addResourceLocation(THRONE_OGRE_MEDIA_DIR "/materials/textures","FileSystem",group);
        resources.addResourceLocation(THRONE_OGRE_MEDIA_DIR "/2.0/scripts/materials/ParticleFX2","FileSystem",group);
        resources.addResourceLocation(THRONE_MEDIA_DIR "/particles","FileSystem",group);
        // Parse only the required resources. The flattened templates retain Scaler on inherited smoke.
        for(const char* resource:{"Particles.material","Smoke.material","Showcase.particle2"}) {
            auto stream=resources.openResource(resource,group);
            Ogre::MaterialManager::getSingleton().parseScript(stream,group);
        }
        // STBI decodes aureola.png as RG8 (luminance, alpha). Sampling A would return 1,
        // making the transparent quad corners opaque. Read alpha from G and replicate R for RGB.
        auto* ring=static_cast<Ogre::HlmsUnlitDatablock*>(root_.getHlmsManager()->getDatablock("Examples/Aureola"));
        ring->setTextureSwizzle(0,Ogre::HlmsUnlitDatablock::R_MASK,Ogre::HlmsUnlitDatablock::R_MASK,
            Ogre::HlmsUnlitDatablock::R_MASK,Ogre::HlmsUnlitDatablock::G_MASK);
        root_.getHlmsManager()->loadBlueNoise();
        particleResourcesLoaded_=true;
    }
    if(script.empty()) return;
    const auto file=project_->resolve(script);
    const auto key=file.generic_u8string();
    if(particleScripts_.count(key)) return;
    if(!std::filesystem::is_regular_file(file)) throw std::runtime_error("Particle script not found: "+key);
    // Custom scripts and their materials/textures share their containing directory.
    const Ogre::String group="ThroneParticles/"+file.parent_path().generic_u8string();
    if(!resources.resourceGroupExists(group)) {
        resources.createResourceGroup(group);
        resources.addResourceLocation(file.parent_path().generic_u8string(),"FileSystem",group);
        resources.initialiseResourceGroup(group,true);
    }
    particleScripts_.insert(key);
}

void Editor::clearParticles() {
    // ParticleFX2 shares particle pools by template: deleting instances alone leaves live particles.
    scene_.destroyAllParticleSystems2();
    for(auto& render:renderObjects_) { render.particles=nullptr; render.particleConfig.reset(); }
    particleSystemCount_=particleCount_=0;
}

void Editor::syncParticles(const LevelExpansion& shown) {
    std::vector<std::optional<ParticleSystemComponent>> desired;
    desired.reserve(renderObjects_.size());
    bool reset=false;
    for(const auto& render:renderObjects_) {
        auto wanted=shown.instances[render.instance].expansion.nodes[render.index].node.particles;
        if(wanted && (!wanted->enabled || (!runtime_ && !particlePreview_))) wanted.reset();
        reset|=wanted!=render.particleConfig;
        desired.push_back(std::move(wanted));
    }
    if(reset) {
        clearParticles();
        for(std::size_t i=0;i<renderObjects_.size();++i) {
            auto& render=renderObjects_[i];
            render.particleConfig=desired[i];
            if(!desired[i]) continue;
            try {
                loadParticleResources(desired[i]->script);
                render.particles=scene_.createParticleSystem2(desired[i]->definition);
                render.particles->setCastShadows(false);
                render.node->attachObject(render.particles);
            } catch(const std::exception& e) { log(std::string("Particle System: ")+e.what()); }
        }
    }
    if(particleResourcesLoaded_ && appliedParticleBlending_!=particleBlending_) {
        for(const char* name:{"Particle/SmokePBS","Particle/SmokeUnlit","Particle/SpherePBS"}) {
            auto* material=root_.getHlmsManager()->getDatablock(name);
            Ogre::HlmsBlendblock blend=*material->getBlendblock();
            blend.setBlendType(particleBlending_==0?Ogre::SBT_TRANSPARENT_ALPHA:Ogre::SBT_REPLACE);
            blend.mAlphaToCoverage=Ogre::HlmsBlendblock::A2cDisabled;
            material->setBlendblock(blend);
            Ogre::HlmsMacroblock macro=*material->getMacroblock();
            macro.mDepthWrite=particleBlending_!=0;
            material->setMacroblock(macro);
            material->setAlphaHashing(particleBlending_!=0);
        }
        appliedParticleBlending_=particleBlending_;
    }
    std::set<const Ogre::ParticleSystemDef*> definitions;
    particleSystemCount_=particleCount_=0;
    for(const auto& render:renderObjects_) if(render.particles) {
        ++particleSystemCount_;
        definitions.insert(render.particles->getParticleSystemDef());
    }
    for(const auto* def:definitions) particleCount_+=def->getNumSimdActiveParticles();
}
}
