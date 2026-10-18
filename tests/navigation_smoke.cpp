#include "navigation_smoke.h"
#include "editor/Editor.h"
#include <OgreCamera.h>
#include <OgreRay.h>
#include <imgui_internal.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace throne {
void navigationSmoke(Editor& editor, Ogre::Camera& camera) {
    const auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    const auto near = [](const Ogre::Vector3& a, const Ogre::Vector3& b) {
        return (a - b).length() < 0.001f;
    };
    ImVec2 mouse(-100, -100);
    bool right = false, middle = false, alt = false, ctrl = false, leftShift = false, rightShift = false;
    float wheel = 0;
    ImGuiKey key = ImGuiKey_None;
    const auto frame = [&] {
        require(editor.frame(1.0f / 60, [&] {
            auto& io = ImGui::GetIO();
            io.ConfigInputTrickleEventQueue = false;
            io.AddMousePosEvent(mouse.x, mouse.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, right);
            io.AddMouseButtonEvent(ImGuiMouseButton_Middle, middle);
            io.AddKeyEvent(ImGuiMod_Alt, alt);
            io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
            io.AddKeyEvent(ImGuiKey_LeftShift, leftShift);
            io.AddKeyEvent(ImGuiKey_RightShift, rightShift);
            io.AddKeyEvent(ImGuiMod_Shift, leftShift || rightShift);
            io.AddMouseWheelEvent(0, wheel);
            for (const auto k : {ImGuiKey_W, ImGuiKey_A, ImGuiKey_S, ImGuiKey_D, ImGuiKey_Q, ImGuiKey_E})
                io.AddKeyEvent(k, key == k);
        }), "Navigation smoke renderer stopped");
        wheel = 0;
    };
    for (int i = 0; i < 3; ++i) frame();
    const auto* window = ImGui::FindWindowByName("Viewport");
    require(window && !window->Collapsed, "Navigation viewport unavailable");
    const auto origin = window->DC.CursorStartPos;
    const ImVec2 size(window->ContentRegionRect.Max.x - origin.x, window->ContentRegionRect.Max.y - origin.y);
    // Aim at an off-centre piece of ground, away from the other sample meshes.
    const auto clip = camera.getProjectionMatrix() * (camera.getViewMatrix() * Ogre::Vector3(-1, 0, 0));
    mouse = ImVec2(origin.x + (clip.x + 1) * .5f * size.x, origin.y + (1 - clip.y) * .5f * size.y);
    frame();
    // ImGui floors mouse coordinates to physical pixels before processing input.
    const auto ray = camera.getCameraToViewportRay((std::floor(mouse.x) - origin.x) / size.x,
                                                   (std::floor(mouse.y) - origin.y) / size.y);
    const auto hit = ray.getPoint(-ray.getOrigin().y / ray.getDirection().y);
    const auto initial = camera.getPosition();
    const auto initialDirection = camera.getDirection();
    middle = true; frame();
    middle = false; frame();
    require(near(initial, camera.getPosition()), "Middle-click moved the camera");
    require(near(camera.getDirection(), (hit - initial).normalisedCopy()), "Middle-click missed the rendered surface");
    require(!near(initialDirection, camera.getDirection()), "Off-centre focus did not turn the camera");
    const auto radius = (initial - hit).length();
    alt = true; middle = true; frame();
    mouse.x += 25; mouse.y += 10; frame();
    require(!near(initial, camera.getPosition()), "Alt-middle drag did not orbit");
    require(std::abs((camera.getPosition() - hit).length() - radius) < .001f, "Orbit changed the focus distance");
    require(near(camera.getDirection(), (hit - camera.getPosition()).normalisedCopy()), "Orbit lost the clicked pivot");
    // The modifier is latched for the gesture, including release before the button.
    alt = false; mouse.x += 10; frame();
    require(std::abs((camera.getPosition() - hit).length() - radius) < .001f, "Orbit switched to pan mid-gesture");
    middle = false; frame();

    const auto orbitPosition = camera.getPosition();
    const auto orbitDirection = camera.getDirection();
    right = true; frame();
    mouse.x += 30; mouse.y -= 15; frame();
    require(near(orbitPosition, camera.getPosition()), "Right-drag orbited instead of looking in place");
    require(!near(orbitDirection, camera.getDirection()), "Right-drag did not turn the camera");
    for (const auto k : {ImGuiKey_W, ImGuiKey_S, ImGuiKey_A, ImGuiKey_D, ImGuiKey_Q, ImGuiKey_E}) {
        const auto position = camera.getPosition();
        const auto direction = camera.getDirection();
        const auto axis = (k == ImGuiKey_Q || k == ImGuiKey_E) ? Ogre::Vector3::UNIT_Y :
            (k == ImGuiKey_W || k == ImGuiKey_S) ? direction : camera.getRight();
        const float sign = (k == ImGuiKey_S || k == ImGuiKey_A || k == ImGuiKey_Q) ? -1.0f : 1.0f;
        key = k; frame();
        require(near(camera.getPosition(), position + axis * (sign * 5.0f / 60)), "Fly movement direction/speed incorrect");
        require(near(direction, camera.getDirection()), "Fly translation changed orientation");
        key = ImGuiKey_None; frame();
    }
    // Flight wheels change persistent base speed without moving the camera by themselves.
    const auto beforeSpeed = camera.getPosition();
    const auto beforeSpeedDirection = camera.getDirection();
    wheel = 1; frame();
    require(near(beforeSpeed, camera.getPosition()) && near(beforeSpeedDirection, camera.getDirection()),
            "Fly wheel dollied or turned the camera");
    const auto advance = [&](float speed) {
        const auto position = camera.getPosition();
        const auto direction = camera.getDirection();
        key = ImGuiKey_W; frame();
        require(near(camera.getPosition(), position + direction * (speed / 60)), "Fly speed/boost incorrect");
        key = ImGuiKey_None; frame();
    };
    advance(6.25f);
    leftShift = true; advance(12.5f);
    leftShift = false; advance(6.25f);
    rightShift = true; advance(6.25f);
    rightShift = false;
    wheel = -1; frame(); advance(5.0f);
    right = false; frame();
    const auto stopped = camera.getPosition();
    key = ImGuiKey_W; frame();
    require(near(stopped, camera.getPosition()), "W moved the camera without right-button capture");
    key = ImGuiKey_None; frame();

    // Empty-space clicks preserve the pivot; the following orbit proves its identity.
    const auto pivot = camera.getPosition() + camera.getDirection() * radius;
    mouse = ImVec2(origin.x + size.x * .95f, origin.y + size.y * .12f);
    frame();
    const auto beforeMiss = camera.getDirection();
    middle = true; frame(); middle = false; frame();
    require(near(beforeMiss, camera.getDirection()) && near(stopped, camera.getPosition()), "Empty-space focus changed the camera");
    alt = true; middle = true; frame(); mouse.x -= 20; frame();
    require(near(camera.getDirection(), (pivot - camera.getPosition()).normalisedCopy()), "Empty-space focus replaced the pivot");
    middle = false; alt = false; frame();
    const auto beforePan = camera.getPosition();
    const auto panDirection = camera.getDirection();
    middle = true; frame(); mouse.x -= 25; frame(); middle = false; frame();
    require(!near(beforePan, camera.getPosition()) && near(panDirection, camera.getDirection()), "Middle drag did not pan");
    const auto dollyStart = camera.getPosition();
    const auto dollyDirection = camera.getDirection();
    const auto dollyPivot = dollyStart + dollyDirection * radius;
    ctrl = true; alt = true; middle = true; frame();
    mouse.y += 20; frame();
    require(near(camera.getPosition(), dollyPivot - dollyDirection * (radius * std::exp(.2f))),
            "Ctrl-Alt-middle vertical drag did not dolly");
    require(near(dollyDirection, camera.getDirection()), "Dolly rotated the camera");
    const auto afterDolly = camera.getPosition();
    mouse.x -= 20; frame();
    require(near(afterDolly, camera.getPosition()), "Horizontal dolly drag moved the camera");
    ctrl = false; alt = false; mouse.y -= 20; frame();
    require(near(dollyStart, camera.getPosition()), "Dolly direction or modifier capture incorrect");
    middle = false; frame();
    require(near(dollyStart, camera.getPosition()) && near(dollyDirection, camera.getDirection()),
            "Dolly release changed focus");
    wheel = 1; frame();
    require(near(camera.getPosition(), dollyPivot - dollyDirection * (radius * .88f)),
            "Normal wheel dolly did not resume after flight");
    std::puts("Navigation smoke passed: focus hit/miss, orbit, look, WASD, pan, dolly, fly speed and Left Shift boost");
}
}
