#include "editor/PbsExtensions.h"
#include <CommandBuffer/OgreCbShaderBuffer.h>
#include <CommandBuffer/OgreCommandBuffer.h>
#include <OgreCamera.h>
#include <OgreHlms.h>
#include <OgreRenderPassDescriptor.h>
#include <OgreRenderQueue.h>
#include <OgreRootLayout.h>
#include <Vao/OgreConstBufferPacked.h>
#include <Vao/OgreReadOnlyBufferPacked.h>
#include <Vao/OgreVaoManager.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <algorithm>
#include <cstring>

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

namespace {
/// Read-only buffer slot of prevWorldBuf (media/pbs); HlmsPbs's world matrices take slot 0.
constexpr Ogre::uint16 prevWorldSlot = 1;
/// float4 rows per draw: the 4x3 world matrix and one unused row.
constexpr std::size_t prevWorldRows = 4;
}

ThronePbs::ThronePbs(Ogre::Archive* dataFolder, Ogre::ArchiveVec* libraryFolders) : HlmsPbs(dataFolder, libraryFolders) {
    // Slot 0 holds the world matrices; prevWorldBuf takes slot 1, so everything after moves on.
    mReservedTexBufferSlots = prevWorldSlot + 1;
}

Ogre::HlmsCache ThronePbs::preparePassHash(const Ogre::CompositorShadowNode* shadowNode, bool casterPass,
                                           bool dualParaboloid, Ogre::SceneManager* sceneManager) {
    auto cache = HlmsPbs::preparePassHash(shadowNode, casterPass, dualParaboloid, sceneManager);
    motionPass_ = writesMotion(casterPass, sceneManager);
    if (motionPass_) {
        ++frame_;
        // Forget objects not drawn in the previous frame (destroyed, or out of view).
        for (auto it = motion_.begin(); it != motion_.end();)
            it = it->second.frame + 1 < frame_ ? motion_.erase(it) : std::next(it);
    }
    return cache;
}

Ogre::uint32 ThronePbs::fillBuffersForV2(const Ogre::HlmsCache* cache, const Ogre::QueuedRenderable& queuedRenderable,
                                         bool casterPass, Ogre::uint32 lastCacheHash, Ogre::CommandBuffer* commandBuffer) {
    const auto instance = HlmsPbs::fillBuffersForV2(cache, queuedRenderable, casterPass, lastCacheHash, commandBuffer);
    if (!motionPass_) return instance;
    // HlmsPbs rebinds its buffers when the Hlms type changes; ours goes with them.
    if (OGRE_EXTRACT_HLMS_TYPE_FROM_CACHE_HASH(lastCacheHash) != mType) bind(commandBuffer);
    const auto* constBuffer = mConstBuffers[mCurrentConstBuffer];
    if (!current_ || currentConstBuffer_ != constBuffer) {
        // A new HlmsPbs const buffer restarts drawId from 0, so a new buffer starts too.
        unmap();
        // Shadow passes may have used the first const buffers already.
        if (mCurrentConstBuffer >= buffers_.size()) buffers_.resize(mCurrentConstBuffer + 1, nullptr);
        auto*& buffer = buffers_[mCurrentConstBuffer];
        if (!buffer)
            // HlmsPbs keeps one uint4 per draw in its const buffer; we keep four float4.
            buffer = mVaoManager->createReadOnlyBuffer(Ogre::PFG_RGBA32_FLOAT, constBuffer->getTotalSizeBytes() * prevWorldRows,
                                                       Ogre::BT_DYNAMIC_PERSISTENT, nullptr, false);
        current_ = buffer;
        mapped_ = static_cast<float*>(current_->map(0, current_->getNumElements()));
        currentConstBuffer_ = constBuffer;
        bind(commandBuffer);
    }
    const auto* object = queuedRenderable.movableObject;
    const auto& world = object->_getParentNodeFullTransform();
    auto& motion = motion_[object];
    if (motion.frame != frame_) {
        motion.previous = motion.frame + 1 == frame_ ? motion.current : world;
        motion.current = world;
        motion.frame = frame_;
    }
    // The rows of the 4x3 matrix, as HlmsPbs writes the world matrix.
    std::memcpy(mapped_ + std::size_t(instance) * prevWorldRows * 4, &motion.previous, 12 * sizeof(float));
    return instance;
}

void ThronePbs::setupRootLayout(Ogre::RootLayout& rootLayout, size_t tid) {
    HlmsPbs::setupRootLayout(rootLayout, tid);
    if (getProperty(tid, motionVectors)) {
        auto& range = rootLayout.mDescBindingRanges[0][Ogre::DescBindingTypes::ReadOnlyBuffer];
        range.end = std::max<Ogre::uint16>(range.end, prevWorldSlot + 1);
    }
}

void ThronePbs::preCommandBufferExecution(Ogre::CommandBuffer* commandBuffer) {
    unmap();
    HlmsPbs::preCommandBufferExecution(commandBuffer);
}

void ThronePbs::frameEnded() {
    HlmsPbs::frameEnded();
    current_ = nullptr;
    currentConstBuffer_ = nullptr;
    mapped_ = nullptr;
}

void ThronePbs::_changeRenderSystem(Ogre::RenderSystem* newRs) {
    unmap();
    if (mVaoManager)
        for (auto* buffer : buffers_)
            if (buffer) mVaoManager->destroyReadOnlyBuffer(buffer);
    buffers_.clear();
    HlmsPbs::_changeRenderSystem(newRs);
}

void ThronePbs::unmap() {
    if (!current_) return;
    current_->unmap(Ogre::UO_KEEP_PERSISTENT, 0, current_->getNumElements());
    current_ = nullptr;
    currentConstBuffer_ = nullptr;
    mapped_ = nullptr;
}

void ThronePbs::bind(Ogre::CommandBuffer* commandBuffer) const {
    if (current_)
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() =
            Ogre::CbShaderBuffer(Ogre::VertexShader, prevWorldSlot, current_, 0, Ogre::uint32(current_->getTotalSizeBytes()));
}
}
