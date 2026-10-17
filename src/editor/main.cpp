#include "audio/AudioSystem.h"
#include "editor/Editor.h"
#include "runtime/PhysicsWorld.h"
#include "script/ScriptRuntime.h"
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
#include <filesystem>
#include <fstream>
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
    // --dump-host-api [file]: the API scripts see, for the L^ language server; no window is opened.
    if (argc > 1 && std::strcmp(argv[1], "--dump-host-api") == 0) {
        const char* path = argc > 2 ? argv[2] : "lhat-host.json";
        const auto json = throne::ScriptRuntime::hostApi();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.write(json.data(), static_cast<std::streamsize>(json.size()))) {
            std::fprintf(stderr, "Could not write %s\n", path);
            return 1;
        }
        std::printf("wrote %s (%zu bytes)\n", path, json.size());
        return 0;
    }
    const bool smoke = argc > 1 && std::strcmp(argv[1], "--smoke") == 0;
    // `ogrethrone <folder>` opens that project; --smoke uses a throwaway one and leaves
    // the user's project list alone; otherwise the launcher is shown.
    std::filesystem::path project, projectList = throne::ProjectList::defaultFile();
    if (smoke) {
        project = std::filesystem::temp_directory_path() / "ogrethrone-smoke";
        std::filesystem::remove_all(project);
        throne::Project::create(project.parent_path(), project.filename().u8string());
        projectList.clear();
    } else if (argc > 1) {
        project = std::filesystem::u8path(argv[1]);
    }
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
            throne::Editor editor(root, *window, *scene, *camera, project, projectList);
            Colibri::LogListener guiLog;
            Colibri::ColibriListener guiPlatform;
            Colibri::ColibriManager gui(&guiLog, &guiPlatform);
            throne::PhysicsWorld physics;
            auto previous = std::chrono::steady_clock::now();
            double accumulator = 0.0;
            unsigned frames = 0;
            bool running = true;
            // One frame; also run from the window hook while a move or resize blocks the loop.
            const auto step = [&] {
                const auto now = std::chrono::steady_clock::now();
                const double elapsed = std::min(0.25, std::chrono::duration<double>(now - previous).count());
                accumulator += elapsed;
                previous = now;
                editor.frame(static_cast<float>(elapsed));
                while (accumulator >= 1.0 / 60.0) {
                    physics.step();
                    accumulator -= 1.0 / 60.0;
                }
                running = root.renderOneFrame();
                ++frames;
            };
            editor.platform().modalFrame = [&] {
                if (running && !window->isClosed()) step();
            };
            while (running && !window->isClosed() && !editor.shouldClose() && (!smoke || frames < 3)) {
                Ogre::WindowEventUtilities::messagePump();
                if (window->isClosed()) break;
                step();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            editor.platform().modalFrame = nullptr;
        }
        ImGui::DestroyContext();
        if (smoke) std::filesystem::remove_all(project);
        root.shutdown();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
