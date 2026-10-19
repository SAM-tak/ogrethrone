#pragma once
#include <cstddef>
#include <memory>
#include <vector>

namespace Ogre { class Window; }
namespace OIS { class InputManager; class JoyStick; }

namespace throne {
/// Owns OIS joystick/gamepad devices associated with an OGRE-created window.
/// Devices are enumerated at startup; capture runs on the window's main thread.
/// Keyboard, mouse and IME events for the editor remain with its UI platform backend.
class OisInput {
public:
    /// Attaches to an existing native window, which must outlive this object.
    /// Throws OIS::Exception if the input system or an attached device cannot be opened.
    explicit OisInput(Ogre::Window& window);
    /// Releases devices and their input manager before the native window is destroyed.
    ~OisInput();
    OisInput(const OisInput&) = delete;
    OisInput& operator=(const OisInput&) = delete;
    /// Polls all startup devices once, before consumers read their states.
    void capture();
    /// Number of joystick/gamepad devices opened at startup; zero is valid.
    std::size_t joystickCount() const { return joysticks_.size(); }
    /// Read-only access to OIS device metadata and its latest JoyStickState.
    /// The index is valid for this object's lifetime; out-of-range indices throw.
    const OIS::JoyStick& joystick(std::size_t index) const;
private:
    /// OIS owns every device and releases them through destroyInputSystem().
    struct DestroyManager { void operator()(OIS::InputManager* manager) const; };
    std::unique_ptr<OIS::InputManager, DestroyManager> manager_;
    std::vector<OIS::JoyStick*> joysticks_;
};
}
