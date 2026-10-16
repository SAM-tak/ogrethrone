#pragma once

#include "AudioSystem.h"
#include "core/Math.h"
#include <memory>

namespace throne {
/// A preloaded mono clip with a fixed listener-relative direction and direct-path
/// parameters. Play with mixer().play(), not play3d(), to avoid double spatialization.
/// Keep the source alive until its voices stop, as with other SoLoud sources.
class SpatialSound : public SoLoud::AudioSource {
public:
    /// Loads a mono clip for binaural playback.
    /// @param audio Audio system providing the Steam Audio context; must outlive this source.
    /// @param filename Mono WAV/Ogg/MP3/FLAC file.
    /// @param direction Listener-relative direction; finite and nonzero, normalized internally.
    /// @param direct Direct-path parameters such as distance attenuation and occlusion.
    /// @throws std::invalid_argument on a zero/non-finite direction or a non-mono/empty clip.
    /// @throws std::runtime_error if decoding or Steam Audio initialization fails.
    SpatialSound(AudioSystem& audio, const char* filename, Vec3 direction,
                 IPLDirectEffectParams direct = {});
    /// Stops all voices playing this source.
    ~SpatialSound() override;
    SpatialSound(const SpatialSound&) = delete;
    SpatialSound& operator=(const SpatialSound&) = delete;
    /// Creates a voice with its own Steam Audio effect state; called by SoLoud on play().
    SoLoud::AudioSourceInstance* createInstance() override;

private:
    struct Data;
    class Instance;
    std::shared_ptr<Data> data_;
};
}
