#include "audio/AudioSystem.h"
#include "editor/SceneEditor.h"
#include "runtime/PhysicsWorld.h"
#include <OgreAbiUtils.h>
#include <OgreArchiveManager.h>
#include <OgreCamera.h>
#include <Compositor/OgreCompositorManager2.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsUnlit.h>
#include <OgreRoot.h>
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

namespace {
/// Registers HlmsUnlit using the shader templates in THRONE_OGRE_MEDIA_DIR.
void registerUnlit(Ogre::Root& root) {
    Ogre::String folder;
    Ogre::StringVector libraries;
    Ogre::HlmsUnlit::getDefaultPaths(folder, libraries);
    auto& archives = Ogre::ArchiveManager::getSingleton();
    const Ogre::String media = THRONE_OGRE_MEDIA_DIR "/";
    Ogre::ArchiveVec libraryArchives;
    for (const auto& library : libraries)
        libraryArchives.push_back(archives.load(media + library, "FileSystem", true));
    auto* unlit = OGRE_NEW Ogre::HlmsUnlit(archives.load(media + folder, "FileSystem", true), &libraryArchives);
    // Debug builds otherwise dump every generated shader into the working directory.
    unlit->setDebugOutputPath(false, false);
    root.getHlmsManager()->registerHlms(unlit);
}
}

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
        camera->setNearClipDistance(0.1f);
        registerUnlit(root);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        {
            throne::SceneEditor editor(root, *window, *scene, *camera);
            Colibri::LogListener guiLog;
            Colibri::ColibriListener guiPlatform;
            Colibri::ColibriManager gui(&guiLog, &guiPlatform);
            throne::PhysicsWorld physics;
            auto previous = std::chrono::steady_clock::now();
            double accumulator = 0.0;
            unsigned frames = 0;
            while (!window->isClosed() && !editor.shouldClose() && (!smoke || frames < 3)) {
                Ogre::WindowEventUtilities::messagePump();
                if (window->isClosed()) break;
                const auto now = std::chrono::steady_clock::now();
                const double elapsed = std::min(0.25, std::chrono::duration<double>(now - previous).count());
                accumulator += elapsed;
                previous = now;
                editor.frame(static_cast<float>(elapsed));
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
