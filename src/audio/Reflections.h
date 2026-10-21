#pragma once
#include <phonon.h>
#include <memory>

namespace throne {
/// Fixed real-time reflection budget. Audio and simulation use the same rate/block size.
inline constexpr int reflectionRate = 48000, reflectionBlock = 512;
inline constexpr int reflectionOrder = 1, reflectionChannels = 4, reflectionSamples = 48000;
inline constexpr unsigned reflectionSourceLimit = 16;
/// The native IR is owned by the completed simulation, never by the audio callback.
struct ReflectionResult {
    IPLReflectionEffectParams parameters{};
    std::shared_ptr<void> owner;
};
}
