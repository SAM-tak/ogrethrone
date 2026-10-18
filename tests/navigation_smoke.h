#pragma once
namespace Ogre { class Camera; }
namespace throne {
class Editor;
/// Exercises viewport mouse and keyboard navigation through real ImGui frames.
void navigationSmoke(Editor& editor, Ogre::Camera& camera);
}
