#include "editor/Environment.h"
#include <OgreDataStream.h>
#include <OgreImage2.h>
#include <OgreTextureBox.h>
#include <OgrePixelFormatGpuUtils.h>
#include <OgreTextureGpu.h>
#include <OgreTextureGpuManager.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace throne {
namespace {
/// Cubemap face edge in texels.
constexpr unsigned faceSize = 512;

/// The unnormalised direction through (s, t) in [-1, 1] of cubemap face @p face.
Ogre::Vector3 faceDirection(unsigned face, float s, float t) {
    switch (face) {
    case 0: return {1, -t, -s};
    case 1: return {-1, -t, s};
    case 2: return {s, 1, t};
    case 3: return {s, -1, -t};
    case 4: return {s, -t, 1};
    default: return {-s, -t, -1};
    }
}
}

std::array<Ogre::Vector3, 9> irradianceHarmonics(const Ogre::Image2& cube) {
    std::array<Ogre::Vector3, 9> harmonics;
    harmonics.fill(Ogre::Vector3::ZERO);
    const auto size = cube.getWidth();
    for (unsigned face = 0; face < 6; ++face)
        for (unsigned y = 0; y < size; ++y)
            for (unsigned x = 0; x < size; ++x) {
                const auto colour = cube.getColourAt(x, y, face);
                const Ogre::Vector3 radiance(colour.r, colour.g, colour.b);
                // Project onto the basis irradianceSH() evaluates, in its (-x, y, z) cubemap axes,
                // weighting each texel by its solid angle.
                const float s = 2 * (x + .5f) / size - 1, t = 2 * (y + .5f) / size - 1;
                const auto n = faceDirection(face, s, t).normalisedCopy();
                const float nx = -n.x, ny = n.y, nz = n.z;
                const float basis[9] = {1, ny, nz, nx, ny * nx, ny * nz, 3 * nz * nz - 1, nz * nx, nx * nx - ny * ny};
                const float weight = 4.0f / float(size * size) / std::pow(1 + s * s + t * t, 1.5f);
                for (int i = 0; i < 9; ++i) harmonics[i] += radiance * (basis[i] * weight);
            }
    // Each band's squared normalisation times its clamped-cosine factor, over pi.
    constexpr float factors[9] = {.0795776f, .159155f, .159155f, .159155f, .298415f, .298415f, .0248681f, .298415f, .0746038f};
    for (int i = 0; i < 9; ++i) harmonics[i] *= factors[i];
    return harmonics;
}

Environment loadEnvironment(Ogre::TextureGpuManager& textures, const std::filesystem::path& file,
                            const std::string& name, float scale, float limit) {
    std::ifstream in(file, std::ios::binary);
    std::vector<char> bytes(std::istreambuf_iterator<char>(in), {});
    if (bytes.empty()) throw std::runtime_error("Could not read " + file.u8string());
    Ogre::DataStreamPtr stream(OGRE_NEW Ogre::MemoryDataStream(bytes.data(), bytes.size(), false, true));
    Ogre::Image2 source;
    source.load(stream, "hdr");
    if (source.getPixelFormat() != Ogre::PFG_RGBA32_FLOAT) throw std::runtime_error(file.u8string() + " is not an HDR image");
    const auto box = source.getData(0);
    const auto width = source.getWidth(), height = source.getHeight();
    const auto texel = [&](unsigned x, unsigned y) {
        const auto* p = static_cast<const float*>(box.at(x % width, (std::min)(y, height - 1), 0));
        return Ogre::Vector3((std::min)(p[0], limit), (std::min)(p[1], limit), (std::min)(p[2], limit)) * scale;
    };
    // Bilinear lookup by world direction: -Z at the image centre, +Y along its top.
    const auto sample = [&](const Ogre::Vector3& d) {
        const float u = (.5f + std::atan2(d.x, -d.z) / (2 * Ogre::Math::PI)) * width - .5f;
        const float v = std::acos(std::clamp(d.y, -1.0f, 1.0f)) / Ogre::Math::PI * height - .5f;
        const float fu = std::floor(u), fv = (std::max)(std::floor(v), 0.0f);
        const float a = u - fu, b = std::clamp(v - fv, 0.0f, 1.0f);
        const auto x = static_cast<unsigned>(fu + width), y = static_cast<unsigned>(fv);
        return (texel(x, y) * (1 - a) + texel(x + 1, y) * a) * (1 - b) +
               (texel(x, y + 1) * (1 - a) + texel(x + 1, y + 1) * a) * b;
    };

    Ogre::Image2 cube;
    cube.createEmptyImage(faceSize, faceSize, 6, Ogre::TextureTypes::TypeCube, Ogre::PFG_RGBA32_FLOAT,
                          Ogre::PixelFormatGpuUtils::getMaxMipmapCount(faceSize, faceSize));
    const auto faces = cube.getData(0);
    for (unsigned face = 0; face < 6; ++face)
        for (unsigned y = 0; y < faceSize; ++y)
            for (unsigned x = 0; x < faceSize; ++x) {
                // Cubemaps are left-handed: their +Z is the world's -Z.
                Ogre::Vector3 radiance = Ogre::Vector3::ZERO;
                for (const float sy : {.25f, .75f})
                    for (const float sx : {.25f, .75f}) {
                        const auto c = faceDirection(face, 2 * (x + sx) / faceSize - 1, 2 * (y + sy) / faceSize - 1).normalisedCopy();
                        radiance += sample({c.x, c.y, -c.z}) * .25f;
                    }
                auto* out = static_cast<float*>(faces.at(x, y, face));
                out[0] = radiance.x, out[1] = radiance.y, out[2] = radiance.z, out[3] = 1;
            }
    Environment result;
    result.harmonics = irradianceHarmonics(cube);

    cube.generateMipmaps(false, Ogre::Image2::FILTER_BILINEAR);
    auto* texture = textures.createTexture(name, Ogre::GpuPageOutStrategy::Discard, Ogre::TextureFlags::ManualTexture,
                                           Ogre::TextureTypes::TypeCube);
    texture->setResolution(faceSize, faceSize);
    texture->setPixelFormat(Ogre::PFG_RGBA32_FLOAT);
    texture->setNumMipmaps(cube.getNumMipmaps());
    texture->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    texture->_setNextResidencyStatus(Ogre::GpuResidency::Resident);
    cube.uploadTo(texture, 0, cube.getNumMipmaps() - 1u);
    result.cubemap = texture;
    return result;
}
}
