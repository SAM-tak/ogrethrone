#include "audio/SpatialSound.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Acoustics {
    IPLScene scene = nullptr;
    IPLStaticMesh mesh = nullptr;
    IPLSimulator simulator = nullptr;
    IPLSource source = nullptr;
    IPLReflectionEffect reflection = nullptr;
    ~Acoustics() {
        iplReflectionEffectRelease(&reflection);
        iplSourceRelease(&source);
        iplSimulatorRelease(&simulator);
        iplStaticMeshRelease(&mesh);
        iplSceneRelease(&scene);
    }
};
}

int main() {
    try {
        throne::AudioSystem audio(true);
        Acoustics a;
        IPLSceneSettings sceneSettings{};
        sceneSettings.type = IPL_SCENETYPE_DEFAULT;
        require(iplSceneCreate(audio.steam(), &sceneSettings, &a.scene) == IPL_STATUS_SUCCESS,
                "Acoustic scene creation failed");
        // An inward-facing room and a small dividing wall at x = 0.
        IPLVector3 vertices[] = {
            {-5,-5,-5}, {5,-5,-5}, {5,5,-5}, {-5,5,-5},
            {-5,-5,5}, {5,-5,5}, {5,5,5}, {-5,5,5},
            {0,-2,-2}, {0,2,-2}, {0,2,2}, {0,-2,2}};
        IPLTriangle triangles[] = {
            {{0,1,2}}, {{0,2,3}}, {{4,7,6}}, {{4,6,5}},
            {{0,4,5}}, {{0,5,1}}, {{3,2,6}}, {{3,6,7}},
            {{0,3,7}}, {{0,7,4}}, {{1,5,6}}, {{1,6,2}},
            {{8,9,10}}, {{8,10,11}}};
        IPLMaterial material{{0.2f,0.3f,0.4f}, 0.5f, {0.1f,0.05f,0.01f}};
        int indices[14]{};
        IPLStaticMeshSettings meshSettings{12,14,1,vertices,triangles,indices,&material};
        require(iplStaticMeshCreate(a.scene, &meshSettings, &a.mesh) == IPL_STATUS_SUCCESS,
                "Acoustic mesh creation failed");
        iplStaticMeshAdd(a.mesh, a.scene);
        iplSceneCommit(a.scene);

        constexpr auto flags = static_cast<IPLSimulationFlags>(
            IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
        IPLSimulationSettings settings{};
        settings.flags = flags;
        settings.sceneType = IPL_SCENETYPE_DEFAULT;
        settings.reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
        settings.maxNumOcclusionSamples = 1;
        settings.maxNumRays = 2048;
        settings.numDiffuseSamples = 32;
        settings.maxDuration = 0.1f;
        settings.maxOrder = 1;
        settings.maxNumSources = 1;
        settings.numThreads = 1;
        settings.samplingRate = 48000;
        settings.frameSize = 512;
        require(iplSimulatorCreate(audio.steam(), &settings, &a.simulator) == IPL_STATUS_SUCCESS,
                "Acoustic simulator creation failed");
        iplSimulatorSetScene(a.simulator, a.scene);
        IPLSourceSettings sourceSettings{flags};
        require(iplSourceCreate(a.simulator, &sourceSettings, &a.source) == IPL_STATUS_SUCCESS,
                "Acoustic source creation failed");
        iplSourceAdd(a.source, a.simulator);
        iplSimulatorCommit(a.simulator);
        IPLSimulationInputs input{};
        input.flags = flags;
        input.directFlags = static_cast<IPLDirectSimulationFlags>(
            IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
        input.source = {{1,0,0}, {0,1,0}, {0,0,-1}, {-1,0,0}};
        input.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST;
        input.numTransmissionRays = 1;
        iplSourceSetInputs(a.source, flags, &input);
        IPLSimulationSharedInputs shared{};
        shared.listener = {{1,0,0}, {0,1,0}, {0,0,-1}, {1,0,0}};
        shared.numRays = 2048;
        shared.numBounces = 4;
        shared.duration = 0.1f;
        shared.order = 1;
        shared.irradianceMinDistance = 1;
        iplSimulatorSetSharedInputs(a.simulator, flags, &shared);
        iplSimulatorRunDirect(a.simulator);
        iplSimulatorRunReflections(a.simulator);
        IPLSimulationOutputs result{};
        iplSourceGetOutputs(a.source, flags, &result);
        require(result.direct.occlusion == 0, "Dividing wall did not occlude the source");
        require(result.direct.transmission[0] > result.direct.transmission[2] &&
                result.direct.transmission[0] < 1, "Wall material transmission was not simulated");

        // Apply the actual simulator output through the production direct/HRTF bridge.
        result.direct.flags = static_cast<IPLDirectEffectFlags>(
            IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
        result.direct.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
        throne::SpatialSound sound(audio, THRONE_AUDIO_FIXTURES "/tone.wav", {-1,0,0}, result.direct);
        const auto voice = audio.mixer().play(sound);
        std::array<float,1024> rendered{};
        double transmittedEnergy = 0;
        for (unsigned block = 0; block < 24; ++block) {
            audio.mixer().mix(rendered.data(), 512);
            for (float sample : rendered) {
                require(std::isfinite(sample), "Nonfinite transmitted audio");
                transmittedEnergy += sample * sample;
            }
        }
        require(voice != 0 && transmittedEnergy > 0, "Wall transmission produced no audio");

        IPLAudioSettings audioSettings{48000,512};
        IPLReflectionEffectSettings effectSettings{IPL_REFLECTIONEFFECTTYPE_CONVOLUTION,4800,4};
        require(iplReflectionEffectCreate(audio.steam(), &audioSettings, &effectSettings,
                &a.reflection) == IPL_STATUS_SUCCESS, "Reflection effect creation failed");
        result.reflections.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
        result.reflections.irSize = 4800;
        result.reflections.numChannels = 4;
        std::array<float,512> impulse{};
        std::array<std::array<float,512>,4> reflected{};
        float* mono = impulse.data();
        float* channels[] = {reflected[0].data(),reflected[1].data(),reflected[2].data(),reflected[3].data()};
        IPLAudioBuffer in{1,512,&mono}, out{4,512,channels};
        double reflectedEnergy = 0;
        impulse[0] = 1;
        for (unsigned block = 0; block < 12; ++block) {
            iplReflectionEffectApply(a.reflection, &result.reflections, &in, &out, nullptr);
            impulse.fill(0);
            for (const auto& channel : reflected) for (float sample : channel) {
                require(std::isfinite(sample), "Nonfinite reflected audio");
                reflectedEnergy += sample * sample;
            }
        }
        require(reflectedEnergy > 1e-12, "Room simulation produced no reflected audio");
        std::printf("Wall occlusion/transmission and room reflection passed; reflected energy %.8g\n",
                    reflectedEnergy);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
