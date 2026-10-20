#include "editor/PbsListeners.h"
#include <OgreCamera.h>
#include <OgreHlms.h>
#include <OgreRenderPassDescriptor.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <algorithm>

namespace throne {
void HlmsListenerChain::add(Ogre::HlmsListener* listener) { listeners_.push_back({listener}); }

void HlmsListenerChain::remove(Ogre::HlmsListener* listener) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                    [&](const Entry& entry) { return entry.listener == listener; }),
                     listeners_.end());
}

Ogre::uint16 HlmsListenerChain::getNumExtraPassTextures(const Ogre::HlmsPropertyVec& properties, bool casterPass) const {
    Ogre::uint16 total = 0;
    for (auto& entry : listeners_) total += entry.textures = entry.listener->getNumExtraPassTextures(properties, casterPass);
    return total;
}

void HlmsListenerChain::propertiesMergedPreGenerationStep(Ogre::Hlms* hlms, const Ogre::HlmsCache& passCache,
                                                          const Ogre::HlmsPropertyVec& renderableCacheProperties,
                                                          const Ogre::PiecesMap renderableCachePieces[Ogre::NumShaderTypes],
                                                          const Ogre::HlmsPropertyVec& properties,
                                                          const Ogre::QueuedRenderable& queuedRenderable, size_t tid) {
    for (auto& entry : listeners_)
        entry.listener->propertiesMergedPreGenerationStep(hlms, passCache, renderableCacheProperties, renderableCachePieces,
                                                          properties, queuedRenderable, tid);
}

void HlmsListenerChain::preparePassHash(const Ogre::CompositorShadowNode* shadowNode, bool casterPass, bool dualParaboloid,
                                        Ogre::SceneManager* sceneManager, Ogre::Hlms* hlms) {
    for (auto& entry : listeners_) entry.listener->preparePassHash(shadowNode, casterPass, dualParaboloid, sceneManager, hlms);
}

Ogre::uint32 HlmsListenerChain::getPassBufferSize(const Ogre::CompositorShadowNode* shadowNode, bool casterPass,
                                                  bool dualParaboloid, Ogre::SceneManager* sceneManager) const {
    Ogre::uint32 size = 0;
    for (const auto& entry : listeners_) size += entry.listener->getPassBufferSize(shadowNode, casterPass, dualParaboloid, sceneManager);
    return size;
}

float* HlmsListenerChain::preparePassBuffer(const Ogre::CompositorShadowNode* shadowNode, bool casterPass,
                                            bool dualParaboloid, Ogre::SceneManager* sceneManager, float* passBufferPtr) {
    for (auto& entry : listeners_)
        passBufferPtr = entry.listener->preparePassBuffer(shadowNode, casterPass, dualParaboloid, sceneManager, passBufferPtr);
    return passBufferPtr;
}

void HlmsListenerChain::hlmsTypeChanged(bool casterPass, Ogre::CommandBuffer* commandBuffer,
                                        const Ogre::HlmsDatablock* datablock, size_t texUnit) {
    for (auto& entry : listeners_) {
        entry.listener->hlmsTypeChanged(casterPass, commandBuffer, datablock, texUnit);
        texUnit += entry.textures;
    }
}

namespace {
const Ogre::IdString motionVectors{"custom_motion_vectors"};

/// Whether this pass writes the G-buffer, where the motion vectors are.
bool writesMotion(bool casterPass, const Ogre::SceneManager* sceneManager) {
    return !casterPass && sceneManager->getCurrentPrePassMode() == Ogre::PrePassCreate;
}
}

void MotionVectors::preparePassHash(const Ogre::CompositorShadowNode*, bool casterPass, bool, Ogre::SceneManager* sceneManager,
                                    Ogre::Hlms* hlms) {
    if (writesMotion(casterPass, sceneManager)) hlms->_setProperty(Ogre::Hlms::kNoTid, motionVectors, 1);
}

Ogre::uint32 MotionVectors::getPassBufferSize(const Ogre::CompositorShadowNode*, bool casterPass, bool,
                                              Ogre::SceneManager* sceneManager) const {
    return writesMotion(casterPass, sceneManager) ? 16u * 4u : 0u;
}

float* MotionVectors::preparePassBuffer(const Ogre::CompositorShadowNode*, bool casterPass, bool,
                                        Ogre::SceneManager* sceneManager, float* out) {
    if (!writesMotion(casterPass, sceneManager)) return out;
    // As HlmsPbs makes passBuf.viewProj, flipped as the target needs.
    const auto* camera = sceneManager->getCamerasInProgress().renderingCamera;
    auto projection = camera->getProjectionMatrixWithRSDepth();
    if (Ogre::Root::getSingleton().getRenderSystem()->getCurrentPassDescriptor()->requiresTextureFlipping())
        for (int column = 0; column < 4; ++column) projection[1][column] = -projection[1][column];
    const auto current = projection * camera->getVrViewMatrix(0);
    auto& previous = previous_.try_emplace(camera, current).first->second;
    for (std::size_t i = 0; i < 16; ++i) *out++ = float(previous[0][i]);
    previous = current;
    return out;
}
}
