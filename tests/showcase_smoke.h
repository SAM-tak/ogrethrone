#pragma once
#include <filesystem>
namespace Ogre { class SceneManager; class Camera; }
namespace throne {
class Editor;
/// Opens all three showcase levels and checks their Vulkan lighting and Play behaviour.
void showcaseSmoke(Editor& editor, Ogre::SceneManager& scene, Ogre::Camera& camera,
                   const std::filesystem::path& captures);
}
