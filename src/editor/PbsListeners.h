#pragma once

#include <OgreHlmsListener.h>
#include <OgreMatrix4.h>
#include <map>
#include <vector>

namespace Ogre {
class Camera;
}

namespace throne {
/// Lets several HlmsListeners extend HlmsPbs, which takes a single listener. Their pass buffer
/// data and extra pass textures follow in the order they were added, so the custom_passBuffer
/// piece (media/pbs) declares their fields in that order too.
class HlmsListenerChain final : public Ogre::HlmsListener {
public:
    /// Appends @p listener, which must stay alive until removed.
    void add(Ogre::HlmsListener* listener);
    /// Removes @p listener.
    void remove(Ogre::HlmsListener* listener);

    Ogre::uint16 getNumExtraPassTextures(const Ogre::HlmsPropertyVec& properties, bool casterPass) const override;
    void propertiesMergedPreGenerationStep(Ogre::Hlms* hlms, const Ogre::HlmsCache& passCache,
                                           const Ogre::HlmsPropertyVec& renderableCacheProperties,
                                           const Ogre::PiecesMap renderableCachePieces[Ogre::NumShaderTypes],
                                           const Ogre::HlmsPropertyVec& properties,
                                           const Ogre::QueuedRenderable& queuedRenderable, size_t tid) override;
    void preparePassHash(const Ogre::CompositorShadowNode* shadowNode, bool casterPass, bool dualParaboloid,
                         Ogre::SceneManager* sceneManager, Ogre::Hlms* hlms) override;
    Ogre::uint32 getPassBufferSize(const Ogre::CompositorShadowNode* shadowNode, bool casterPass, bool dualParaboloid,
                                   Ogre::SceneManager* sceneManager) const override;
    float* preparePassBuffer(const Ogre::CompositorShadowNode* shadowNode, bool casterPass, bool dualParaboloid,
                             Ogre::SceneManager* sceneManager, float* passBufferPtr) override;
    void hlmsTypeChanged(bool casterPass, Ogre::CommandBuffer* commandBuffer, const Ogre::HlmsDatablock* datablock,
                         size_t texUnit) override;

private:
    struct Entry {
        Ogre::HlmsListener* listener;
        /// Its extra pass textures in the pass being prepared, to bind them after the earlier ones.
        Ogre::uint16 textures = 0;
    };
    mutable std::vector<Entry> listeners_;
};

/// Writes screen-space motion vectors from the depth prepass (TDDja/09): sets custom_motion_vectors
/// and hands the vertex shader the camera's view-projection of the previous frame, so the
/// prepass's third target holds, per pixel, the change in UV since then. Objects are taken as
/// unmoved; only the camera's motion is measured.
class MotionVectors final : public Ogre::HlmsListener {
public:
    void preparePassHash(const Ogre::CompositorShadowNode* shadowNode, bool casterPass, bool dualParaboloid,
                         Ogre::SceneManager* sceneManager, Ogre::Hlms* hlms) override;
    Ogre::uint32 getPassBufferSize(const Ogre::CompositorShadowNode* shadowNode, bool casterPass, bool dualParaboloid,
                                   Ogre::SceneManager* sceneManager) const override;
    float* preparePassBuffer(const Ogre::CompositorShadowNode* shadowNode, bool casterPass, bool dualParaboloid,
                             Ogre::SceneManager* sceneManager, float* passBufferPtr) override;

private:
    /// Each camera's view-projection in its last prepass.
    std::map<const Ogre::Camera*, Ogre::Matrix4> previous_;
};
}
