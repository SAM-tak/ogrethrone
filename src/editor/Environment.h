#pragma once

#include <OgreVector3.h>
#include <array>
#include <filesystem>
#include <memory>
#include <string>

namespace Ogre {
class Image2;
class TextureGpu;
class TextureGpuManager;
}

namespace throne {
/// Image-based lighting read from an equirectangular Radiance HDR file: a radiance
/// cubemap for the sky and specular reflections, and the spherical harmonics of
/// its diffuse irradiance.
struct Environment {
    /// Radiance in OGRE's left-handed cubemap space. Its mipmaps are box filtered until
    /// the editor prefilters them for GGX roughness.
    Ogre::TextureGpu* cubemap = nullptr;
    /// Irradiance divided by pi, in the basis HlmsPbs evaluates for AmbientSh.
    std::array<Ogre::Vector3, 9> harmonics;
};

/// An environment as read from its file, before it reaches the GPU.
struct EnvironmentImage {
    /// The radiance cubemap with box-filtered mipmaps.
    std::shared_ptr<Ogre::Image2> cube;
    /// As Environment::harmonics.
    std::array<Ogre::Vector3, 9> harmonics;
};

/// Reads @p file, multiplying its radiance by @p scale (per channel). Radiance above @p limit before
/// scaling (the sun, which a Light component draws) is clamped. Touches no GPU state, so
/// it can run on a worker thread. Throws when the file cannot be read as an HDR image.
EnvironmentImage readEnvironment(const std::filesystem::path& file, const Ogre::Vector3& scale = Ogre::Vector3(1.0f), float limit = 16.0f);
/// Uploads @p image into an Environment whose cubemap is named @p name.
Environment uploadEnvironment(Ogre::TextureGpuManager& textures, const EnvironmentImage& image, const std::string& name);
}
