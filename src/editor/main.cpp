#include "runtime/PhysicsWorld.h"
#include "audio/AudioSystem.h"
#include <OgreAbiUtils.h>
#include <OgreCamera.h>
#include <Compositor/OgreCompositorManager2.h>
#include <OgreRoot.h>
#include <OgreImguiManager.h>
#include <OgreSceneManager.h>
#include <OgreVulkanPlugin.h>
#include <OgreWindow.h>
#include <OgreWindowEventUtilities.h>
#include <ColibriGui/ColibriManager.h>
#include <imgui.h>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>
#include <stdexcept>

int main(int argc, char** argv) {
    const bool smoke = argc > 1 && std::strcmp(argv[1], "--smoke") == 0;
    try {
        throne::AudioSystem audio(smoke);
        const auto abi = Ogre::generateAbiCookie();
        Ogre::VulkanPlugin vulkan;
        Ogre::Root root(&abi, "", "", "OgreThrone.log", "Ogre Throne");
        root.installPlugin(&vulkan, nullptr);
        auto* renderer = root.getRenderSystemByName("Vulkan Rendering Subsystem");
        if (!renderer) throw std::runtime_error("Vulkan renderer unavailable");
        root.setRenderSystem(renderer);
        root.initialise(false);
        auto* window = root.createRenderWindow("Ogre Throne", 1280, 720, false);
        auto* scene = root.createSceneManager(Ogre::ST_GENERIC, 1, "MainScene");
        auto* camera = scene->createCamera("MainCamera");
        camera->setAutoAspectRatio(true);
        camera->setNearClipDistance(0.1f);
        auto* compositor = root.getCompositorManager2();
        compositor->createBasicWorkspaceDef("MainWorkspace", Ogre::ColourValue(0.04f, 0.06f, 0.10f));
        compositor->addWorkspace(scene, window->getTexture(), camera, "MainWorkspace", true);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        // GUI rendering and platform input are wired in the next editor milestone.
        {
            Ogre::ImguiManager editorGui;
            Colibri::ColibriManager gui(nullptr, nullptr);
            throne::PhysicsWorld physics;
            auto previous = std::chrono::steady_clock::now();
            double accumulator = 0.0;
            unsigned frames = 0;
            while (!window->isClosed() && (!smoke || frames < 3)) {
                Ogre::WindowEventUtilities::messagePump();
                if (window->isClosed()) break;
                const auto now = std::chrono::steady_clock::now();
                accumulator += std::min(0.25, std::chrono::duration<double>(now - previous).count());
                previous = now;
                while (accumulator >= 1.0 / 60.0) {
                    physics.step();
                    accumulator -= 1.0 / 60.0;
                }
                if (!root.renderOneFrame()) break;
                ++frames;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        ImGui::DestroyContext();
        root.shutdown();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
