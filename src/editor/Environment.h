#pragma once

#include <OgreVector3.h>
#include <array>
#include <filesystem>
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

/// The spherical harmonics of the diffuse irradiance that a radiance cubemap image
/// (in OGRE's cubemap space, any colour format) gives, divided by pi.
std::array<Ogre::Vector3, 9> irradianceHarmonics(const Ogre::Image2& cube);

/// Reads @p file into an Environment whose cubemap is named @p name, multiplying its
/// radiance by @p scale. Radiance above @p limit before scaling (the sun, which a Light
/// component draws) is clamped. Throws when the file cannot be read as an HDR image.
Environment loadEnvironment(Ogre::TextureGpuManager& textures, const std::filesystem::path& file,
                            const std::string& name, float scale = 1.0f, float limit = 16.0f);
}
