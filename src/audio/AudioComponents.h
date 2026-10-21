#pragma once
#include "core/Math.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <tuple>
#include <cmath>
#include <stdexcept>

namespace throne {
struct AudioSourceComponent {
    bool enabled = true, spatial = true, loop = false, autoPlay = false, streaming = false;
    bool occlusion = true, transmission = true, reflections = false;
    float reflectionVolume = 1;
    std::string clip;
    float volume = 1, minDistance = 1, maxDistance = 100;
    std::uint32_t bus = 1;
    void validate() const {
        const auto path=std::filesystem::u8path(clip);
        if(clip.size()>255 || clip.find('\0')!=std::string::npos || path.is_absolute() || path.has_root_name())
            throw std::runtime_error("Audio clip must be a project-relative path");
        for(const auto& part:path) if(part=="..") throw std::runtime_error("Audio clip escapes project");
        if (!std::isfinite(reflectionVolume) || reflectionVolume<0 || reflectionVolume>10)
            throw std::runtime_error("Invalid reflection volume");
        if (!std::isfinite(volume) || volume < 0 || volume > 100 || !std::isfinite(minDistance) ||
            !std::isfinite(maxDistance) || minDistance < 0 || maxDistance <= minDistance || maxDistance > 1.0e6f || !bus)
            throw std::runtime_error("Invalid AudioSource volume, distance or bus");
    }
    bool operator==(const AudioSourceComponent& b) const {
        return std::tie(enabled,spatial,loop,autoPlay,streaming,occlusion,transmission,reflections,reflectionVolume,clip,volume,minDistance,maxDistance,bus) ==
               std::tie(b.enabled,b.spatial,b.loop,b.autoPlay,b.streaming,b.occlusion,b.transmission,b.reflections,b.reflectionVolume,b.clip,b.volume,b.minDistance,b.maxDistance,b.bus);
    }
};
struct AudioListenerComponent {
    bool enabled = true;
    int priority = 0;
    bool operator==(const AudioListenerComponent& b) const { return enabled == b.enabled && priority == b.priority; }
};
/// Acoustic participation is authored independently of rendering and collision.
enum class AcousticShape { Render, Box };
struct AcousticGeometryComponent {
    bool enabled = true;
    AcousticShape shape = AcousticShape::Render;
    Vec3 boxSize{1,1,1};
    Vec3 absorption{.2f,.3f,.4f};
    float scattering = .5f;
    Vec3 transmission{.1f,.05f,.01f};
    void validate() const {
        if(shape!=AcousticShape::Render && shape!=AcousticShape::Box) throw std::runtime_error("Invalid acoustic shape");
        for(float v:{boxSize.x,boxSize.y,boxSize.z})
            if(!std::isfinite(v) || v<.001f || v>1.0e6f) throw std::runtime_error("Invalid acoustic box dimensions");
        for(float v:{absorption.x,absorption.y,absorption.z,scattering,transmission.x,transmission.y,transmission.z})
            if(!std::isfinite(v) || v<0 || v>1) throw std::runtime_error("Acoustic coefficients must be between 0 and 1");
    }
    bool operator==(const AcousticGeometryComponent& b) const {
        return enabled==b.enabled && shape==b.shape && boxSize==b.boxSize && absorption==b.absorption &&
               scattering==b.scattering && transmission==b.transmission;
    }
};
enum class AudioCommand { Play, Pause, Stop };
}
