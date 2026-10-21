#pragma once

#include <OgreHlmsListener.h>
#include <OgreHlmsPbs.h>
#include <OgreMatrix4.h>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace Ogre {
class Camera;
class ReadOnlyBufferPacked;
}

namespace throne {
/// Lets several HlmsListeners extend HlmsPbs, which takes a single listener. Their pass buffer
/// data and extra pass textures follow in the order they were added, so the custom_passBuffer
/// piece (media/pbs) declares their fields in that order too.
class HlmsListenerChain final : public Ogre::HlmsListener {
public:
    /// Set to a listener's first extra pass texture slot while its propertiesMergedPreGenerationStep runs.
    static inline const Ogre::IdString firstTexture{"custom_first_texture"};

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
/// prepass's third target holds, per pixel, the change in UV since then. ThronePbs adds each
/// object's world matrix of the previous frame.
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

/// Screen-space ambient occlusion in the passes that use the depth prepass (TDDja/09): sets
/// custom_ssao and binds @ref texture, the occlusion computed from the prepass, which the
/// AmbientOcclusion piece multiplies the indirect light by.
class AmbientOcclusion final : public Ogre::HlmsListener {
public:
    explicit AmbientOcclusion(Ogre::HlmsManager& manager);
    ~AmbientOcclusion();
    AmbientOcclusion(const AmbientOcclusion&) = delete;
    AmbientOcclusion& operator=(const AmbientOcclusion&) = delete;

    /// The occlusion of the current viewport targets; null while there are none.
    Ogre::TextureGpu* texture = nullptr;

    Ogre::uint16 getNumExtraPassTextures(const Ogre::HlmsPropertyVec& properties, bool casterPass) const override;
    void propertiesMergedPreGenerationStep(Ogre::Hlms* hlms, const Ogre::HlmsCache& passCache,
                                           const Ogre::HlmsPropertyVec& renderableCacheProperties,
                                           const Ogre::PiecesMap renderableCachePieces[Ogre::NumShaderTypes],
                                           const Ogre::HlmsPropertyVec& properties,
                                           const Ogre::QueuedRenderable& queuedRenderable, size_t tid) override;
    void preparePassHash(const Ogre::CompositorShadowNode* shadowNode, bool casterPass, bool dualParaboloid,
                         Ogre::SceneManager* sceneManager, Ogre::Hlms* hlms) override;
    void hlmsTypeChanged(bool casterPass, Ogre::CommandBuffer* commandBuffer, const Ogre::HlmsDatablock* datablock,
                         size_t texUnit) override;

private:
    Ogre::HlmsManager& manager_;
    const Ogre::HlmsSamplerblock* sampler_ = nullptr;
    /// Whether the pass being drawn reads the occlusion.
    bool active_ = false;
};

/// HlmsPbs with each object's world matrix of the previous frame, for the motion vectors: in
/// depth prepasses it writes it per draw into a read-only buffer at slot 1 (prevWorldBuf), a
/// slot it reserves in every pass. An object drawn for the first time, or not drawn in the
/// previous frame, is taken as unmoved. Skinned and posed meshes are not handled.
class ThronePbs final : public Ogre::HlmsPbs {
public:
    ThronePbs(Ogre::Archive* dataFolder, Ogre::ArchiveVec* libraryFolders);

    Ogre::HlmsCache preparePassHash(const Ogre::CompositorShadowNode* shadowNode, bool casterPass, bool dualParaboloid,
                                    Ogre::SceneManager* sceneManager) override;
    Ogre::uint32 fillBuffersForV2(const Ogre::HlmsCache* cache, const Ogre::QueuedRenderable& queuedRenderable,
                                  bool casterPass, Ogre::uint32 lastCacheHash, Ogre::CommandBuffer* commandBuffer) override;
    void setupRootLayout(Ogre::RootLayout& rootLayout, size_t tid) override;
    void preCommandBufferExecution(Ogre::CommandBuffer* commandBuffer) override;
    void frameEnded() override;
    void _changeRenderSystem(Ogre::RenderSystem* newRs) override;

private:
    /// An object's world matrix in the last two prepass frames it was drawn in.
    struct Motion {
        Ogre::Matrix4 previous, current;
        std::uint64_t frame = 0;
    };
    /// Unmaps the buffer being written, keeping it for the frame.
    void unmap();
    /// Binds the buffer being written, if any.
    void bind(Ogre::CommandBuffer* commandBuffer) const;

    std::unordered_map<const Ogre::MovableObject*, Motion> motion_;
    /// Counts depth prepasses; one per frame.
    std::uint64_t frame_ = 0;
    bool motionPass_ = false;
    /// One buffer per HlmsPbs const buffer: drawId restarts with each.
    std::vector<Ogre::ReadOnlyBufferPacked*> buffers_;
    Ogre::ReadOnlyBufferPacked* current_ = nullptr;
    const Ogre::ConstBufferPacked* currentConstBuffer_ = nullptr;
    float* mapped_ = nullptr;
};
}
