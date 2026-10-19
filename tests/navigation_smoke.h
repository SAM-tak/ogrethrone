#pragma once
#include <filesystem>
namespace Ogre { class Camera; class Window; }
namespace throne {
class Editor;
/// Exercises viewport mouse and keyboard navigation through real ImGui frames.
void navigationSmoke(Editor& editor, Ogre::Window& window, Ogre::Camera& camera,const std::filesystem::path& project);
}
