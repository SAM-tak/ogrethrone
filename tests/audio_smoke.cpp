#include "audio/SpatialSound.h"
#include <soloud_wav.h>
#include <soloud_wavstream.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

double render(throne::AudioSystem& audio, SoLoud::AudioSource& sound) {
    auto& mixer = audio.mixer();
    mixer.stopAll();
    const auto voice = mixer.play(sound);
    require(voice != 0, "Cannot start audio voice");
    std::array<float, 1024> output{};
    double energy = 0;
    unsigned blocks = 0;
    do {
        mixer.mix(output.data(), 512);
        for (float sample : output) {
            require(std::isfinite(sample), "Nonfinite audio output");
            energy += double(sample) * sample;
        }
    } while (mixer.isValidVoiceHandle(voice) && ++blocks < 200);
    require(!mixer.isValidVoiceHandle(voice), "Audio voice did not finish");
    return energy;
}
}

int main(int argc, char** argv) {
    try {
        const bool device = argc > 1 && std::strcmp(argv[1], "--device") == 0;
        throne::AudioSystem audio(!device);
        const std::string fixture = THRONE_AUDIO_FIXTURES "/tone.wav";
        if (device) {
            throne::SpatialSound sound(audio, fixture.c_str(), {1, 0, -1});
            const auto voice = audio.mixer().play(sound, 0.25f);
            require(voice != 0, "Cannot start device audio voice");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (audio.mixer().isValidVoiceHandle(voice) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            require(!audio.mixer().isValidVoiceHandle(voice), "Device playback did not advance");
            std::puts("Audio device initialized; spatial voice completed");
            return 0;
        }
        for (const char* extension : {"wav", "ogg", "mp3"}) {
            const auto path = std::string(THRONE_AUDIO_FIXTURES "/tone.") + extension;
            SoLoud::Wav sample;
            require(sample.load(path.c_str()) == SoLoud::SO_NO_ERROR, "Audio decode failed");
            require(render(audio, sample) > 0.001, "Decoded clip was silent");
            SoLoud::WavStream stream;
            require(stream.load(path.c_str()) == SoLoud::SO_NO_ERROR, "Audio stream load failed");
            require(render(audio, stream) > 0.001, "Streamed clip was silent");
            throne::SpatialSound spatial(audio, path.c_str(), {1, 0, -1});
            require(render(audio, spatial) > 0.001, "Spatial clip was silent");
        }

        throne::SpatialSound clear(audio, fixture.c_str(), {1, 0, 0});
        const auto clearEnergy = render(audio, clear);
        IPLDirectEffectParams direct{};
        direct.flags = IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION;
        direct.occlusion = 0.2f;
        throne::SpatialSound occluded(audio, fixture.c_str(), {1, 0, 0}, direct);
        const auto occludedEnergy = render(audio, occluded);
        require(occludedEnergy > 0 && occludedEnergy < clearEnergy * 0.1,
                "Steam Audio occlusion did not attenuate the clip");

        auto instance = std::unique_ptr<SoLoud::AudioSourceInstance>(clear.createInstance());
        instance->init(clear, 0);
        std::array<float, 1024> planar{};
        double difference = 0;
        for (unsigned i = 0; i < 8; ++i) {
            instance->getAudio(planar.data(), 512, 512);
            for (unsigned j = 0; j < 512; ++j)
                difference += std::abs(planar[j] - planar[512 + j]);
        }
        require(difference > 0.01, "HRTF did not produce different ear signals");

        // SDK blocks are fixed-size; consumers may request different chunk sizes.
        const auto chunks = [&](unsigned chunk) {
            auto reader = std::unique_ptr<SoLoud::AudioSourceInstance>(clear.createInstance());
            reader->init(clear, 0);
            std::vector<float> result;
            std::vector<float> buffer(chunk * 2);
            unsigned total = 0;
            while (total < 4096) {
                const auto n = (std::min)(chunk, 4096u - total);
                require(reader->getAudio(buffer.data(), n, chunk) == n, "Short spatial PCM read");
                for (unsigned j = 0; j < n; ++j) {
                    result.push_back(buffer[j]);
                    result.push_back(buffer[chunk + j]);
                }
                total += n;
            }
            return result;
        };
        require(chunks(31) == chunks(777), "Spatial output changed with consumer buffer size");
        std::puts("WAV/Ogg/MP3 decode, streaming, HRTF, occlusion and voice completion passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
