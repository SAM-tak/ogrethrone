#pragma once

#include "AudioSystem.h"
#include <memory>

namespace throne {
// A preloaded mono clip with a fixed listener-relative direction and direct-path
// parameters. Play with mixer().play(), not play3d(), to avoid double spatialization.
// Keep the source alive until its voices stop, as with other SoLoud sources.
class SpatialSound : public SoLoud::AudioSource {
public:
    SpatialSound(AudioSystem& audio, const char* filename, IPLVector3 direction,
                 IPLDirectEffectParams direct = {});
    ~SpatialSound() override;
    SpatialSound(const SpatialSound&) = delete;
    SpatialSound& operator=(const SpatialSound&) = delete;
    SoLoud::AudioSourceInstance* createInstance() override;

private:
    struct Data;
    class Instance;
    std::shared_ptr<Data> data_;
};
}
