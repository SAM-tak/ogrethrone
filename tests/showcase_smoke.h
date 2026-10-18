#pragma once
#include <filesystem>
namespace Ogre { class SceneManager; class Camera; class Window; }
namespace throne {
class Editor;
/// Sends a native close request during Play and checks that no further frame runs.
void closePlayingSmoke(Editor& editor, Ogre::Window& window);
/// Opens all four showcases and checks rendering, viewport presentation and Play behaviour.
void showcaseSmoke(Editor& editor, Ogre::Window& window, Ogre::SceneManager& scene, Ogre::Camera& camera,
                   const std::filesystem::path& captures);
}
