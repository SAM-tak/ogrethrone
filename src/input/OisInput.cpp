#include "input/OisInput.h"
#include <OIS.h>
#include <OgreWindow.h>
#include <OgreLogManager.h>
#include <cstdint>
#include <string>

namespace throne {
void OisInput::DestroyManager::operator()(OIS::InputManager* manager) const {
    OIS::InputManager::destroyInputSystem(manager);
}

OisInput::OisInput(Ogre::Window& window) {
    std::uintptr_t handle = 0;
#if defined(__linux__)
    // OGRE's Vulkan backend exposes an XCB window ID; OIS uses it with X11.
    std::uint32_t xwindow = 0;
    window.getCustomAttribute("xcb_window_t", &xwindow);
    handle = xwindow;
#else
    void* native = nullptr;
    window.getCustomAttribute("WINDOW", &native);
    handle = reinterpret_cast<std::uintptr_t>(native);
#endif
    OIS::ParamList parameters;
    parameters.emplace("WINDOW", std::to_string(handle));
    // DirectInput devices coexist with editor UI without taking exclusive input.
    parameters.emplace("w32_joystick", "DISCL_FOREGROUND");
    parameters.emplace("w32_joystick", "DISCL_NONEXCLUSIVE");
    manager_.reset(OIS::InputManager::createInputSystem(parameters));
    const auto count = manager_->getNumberOfDevices(OIS::OISJoyStick);
    for (int i = 0; i < count; ++i)
        joysticks_.push_back(static_cast<OIS::JoyStick*>(manager_->createInputObject(OIS::OISJoyStick, false)));
    auto& log = Ogre::LogManager::getSingleton();
    log.logMessage("Input: OIS " + manager_->getVersionName() + " / " + manager_->inputSystemName() +
                   "; joysticks/gamepads: " + std::to_string(joysticks_.size()));
    for (const auto* joystick : joysticks_) log.logMessage("Input device: " + joystick->vendor());
}

OisInput::~OisInput() = default;

void OisInput::capture() {
    for (auto* joystick : joysticks_) joystick->capture();
}

const OIS::JoyStick& OisInput::joystick(std::size_t index) const { return *joysticks_.at(index); }
}
