#pragma once

#include "AudioSystem.h"
#include "Reflections.h"
#include "core/Math.h"
#include <memory>

namespace SoLoud { class Wav; }

namespace throne {
/// A preloaded mono clip with an updatable listener-relative direction and direct-path
/// parameters and optional reflected/reverberant sound. Imported PCM is resampled
/// to 48 kHz outside the callback. A reflection IR has one consuming voice.
/// Play with mixer().play(), not play3d(), to avoid double spatialization.
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
    SpatialSound(AudioSystem& audio, std::shared_ptr<SoLoud::Wav> clip, Vec3 direction,
                 IPLDirectEffectParams direct = {});
    /// Single simulation writer; the audio reader never waits for a parameter update.
    void update(Vec3 direction, float distanceGain);
    /// Publishes the complete direct-path result together with its direction.
    void update(Vec3 direction, const IPLDirectEffectParams& direct);
    /// Owns native IRs on the simulation side; callback reclamation never releases a simulator.
    void updateReflections(std::shared_ptr<const ReflectionResult> result,float gain,IPLCoordinateSpace3 listener);
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
