#include "AudioSystem.h"
#include <stdexcept>
#include <string>

namespace throne {
AudioSystem::AudioSystem(bool offline) {
    static_assert(STEAMAUDIO_VERSION == 0x040801, "Use the pinned Steam Audio SDK");
    IPLContextSettings settings{};
    settings.version = STEAMAUDIO_VERSION;
    if (iplContextCreate(&settings, &context_) != IPL_STATUS_SUCCESS)
        throw std::runtime_error("Steam Audio context creation failed");
    const auto result = mixer_.init(SoLoud::Soloud::CLIP_ROUNDOFF,
        offline ? SoLoud::Soloud::NULLDRIVER : SoLoud::Soloud::MINIAUDIO, 48000, 512, 2);
    if (result != SoLoud::SO_NO_ERROR) {
        iplContextRelease(&context_);
        throw std::runtime_error(std::string("Audio output initialization failed: ") +
                                 mixer_.getErrorString(result));
    }
}

AudioSystem::~AudioSystem() {
    mixer_.deinit();
    iplContextRelease(&context_);
}
}
