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
#include <OgreHlmsPbs.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreForwardPlusBase.h>
#include <OgreVulkanPlugin.h>
#include <OgreWindow.h>
#include <OgreTextureGpu.h>
#include <OgreWindowEventUtilities.h>
#include <ColibriGui/ColibriManager.h>
#include <imgui.h>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <stdexcept>
#include <array>
#ifdef THRONE_NAVIGATION_SMOKE
#include "navigation_smoke.h"
#include "showcase_smoke.h"
#endif

namespace {
/// Registers an HLMS implementation from the pinned OGRE shader templates.
template <typename Hlms>
void registerHlms(Ogre::Root& root) {
    Ogre::String folder;
    Ogre::StringVector libraries;
    Hlms::getDefaultPaths(folder, libraries);
    auto& archives = Ogre::ArchiveManager::getSingleton();
    const Ogre::String media = THRONE_OGRE_MEDIA_DIR "/";
    Ogre::ArchiveVec libraryArchives;
    for (const auto& library : libraries)
        libraryArchives.push_back(archives.load(media + library, "FileSystem", true));
    auto* hlms = OGRE_NEW Hlms(archives.load(media + folder, "FileSystem", true), &libraryArchives);
    // Debug builds otherwise dump every generated shader into the working directory.
    hlms->setDebugOutputPath(false, false);
    root.getHlmsManager()->registerHlms(hlms);
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
    const bool physicsSmoke = argc > 1 && std::strcmp(argv[1], "--smoke-physics") == 0;
    const bool profilePhysics = argc > 1 && std::strcmp(argv[1], "--profile-physics") == 0;
    bool navigationSmoke = false, showcaseSmoke = false;
#ifdef THRONE_NAVIGATION_SMOKE
    showcaseSmoke = argc > 1 && std::strcmp(argv[1], "--smoke-showcases") == 0;
    navigationSmoke = argc > 1 && std::strcmp(argv[1], "--smoke-navigation") == 0;
#endif
    const bool smoke = showcaseSmoke || physicsSmoke || profilePhysics || navigationSmoke || (argc > 1 && std::strcmp(argv[1], "--smoke") == 0);
    const auto captures = physicsSmoke ? std::filesystem::absolute(argc > 3 ? argv[3] : "out/physics-captures") : std::filesystem::path{};
    // `ogrethrone <folder>` opens that project; --smoke uses a throwaway one and leaves
    // the user's project list alone; otherwise the launcher is shown.
    std::filesystem::path project, projectList = throne::ProjectList::defaultFile();
    if (smoke) {
        project = std::filesystem::temp_directory_path() / (showcaseSmoke ? "ogrethrone-showcase-smoke" : profilePhysics ? "ogrethrone-physics-profile" : navigationSmoke ? "ogrethrone-navigation-smoke" :
            physicsSmoke ? "ogrethrone-physics-smoke" : "ogrethrone-smoke");
        std::filesystem::remove_all(project);
        if (physicsSmoke || profilePhysics || navigationSmoke || showcaseSmoke) {
            const auto source=std::filesystem::absolute(argc>2?argv[2]:showcaseSmoke?"samples/showcase":"samples/physics");
            std::filesystem::copy(source,project,std::filesystem::copy_options::recursive);
            if (physicsSmoke) std::filesystem::create_directories(captures);
        } else throne::Project::create(project.parent_path(), project.filename().u8string());
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
        Ogre::NameValuePairList windowParameters;
        if (profilePhysics) windowParameters["vsync"] = "false";
#ifdef NDEBUG
        constexpr auto windowTitle = "Ogre Throne";
#else
        constexpr auto windowTitle = "Ogre Throne [Debug]";
#endif
        auto* window = root.createRenderWindow(windowTitle, 1280, 720, false, &windowParameters);
        auto* scene = root.createSceneManager(Ogre::ST_GENERIC, 1, "MainScene");
        auto* camera = scene->createCamera("MainCamera");
        camera->setNearClipDistance(0.1f);
        registerHlms<Ogre::HlmsUnlit>(root);
        registerHlms<Ogre::HlmsPbs>(root);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        {
            throne::Editor editor(root, *window, *scene, *camera, project, projectList);
            if (physicsSmoke && (!scene->getForwardPlus() ||
                scene->getForwardPlus()->getForwardPlusMethod()!=Ogre::ForwardPlusBase::MethodForwardClustered))
                throw std::runtime_error("The viewport is not using Forward Clustered");
            Colibri::LogListener guiLog;
            Colibri::ColibriListener guiPlatform;
            Colibri::ColibriManager gui(&guiLog, &guiPlatform);
#ifdef THRONE_NAVIGATION_SMOKE
            if (navigationSmoke) throne::navigationSmoke(editor, *camera);
            if (showcaseSmoke) throne::showcaseSmoke(editor,*scene,*camera,std::filesystem::absolute(argc>3?argv[3]:"out/showcase-captures"));
#endif
            auto previous = std::chrono::steady_clock::now();
            unsigned frames = 0;
            bool running = !navigationSmoke && !showcaseSmoke, stepping = false;
            std::ofstream profile;
            std::array<double, 10> totals{};
            unsigned profileSamples = 0;
            if (profilePhysics) {
                const auto path = std::filesystem::path(argc > 3 ? argv[3] : "out/physics-profile.csv");
                if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
                profile.open(path);
                if (!profile) throw std::runtime_error("Could not open physics profile CSV");
                profile << "frame,nodes,bodies,frame_ms,ui_ms,script_ms,snapshot_ms,physics_apply_ms,physics_prepare_ms,physics_step_ms,render_sync_ms,render_present_ms,physics_wait_ms\n";
            }
            // One frame; also run from the window hook while a move or resize blocks the loop.
            const auto step = [&] {
                if (stepping) return;
                stepping=true;
                struct Guard { bool& value; ~Guard() { value=false; } } guard{stepping};
                const auto now = std::chrono::steady_clock::now();
                const double elapsed = (physicsSmoke || profilePhysics) ? 1.0/60.0 : std::min(0.25, std::chrono::duration<double>(now - previous).count());
                previous = now;
                running = editor.frame(static_cast<float>(elapsed));
                ++frames;
            };
            editor.platform().modalFrame = [&] {
                if (running && !window->isClosed()) step();
            };
            while (running && !window->isClosed() && !editor.shouldClose() && (!smoke || frames < (profilePhysics ? 2100u : physicsSmoke ? 245u : 3u))) {
                Ogre::WindowEventUtilities::messagePump();
                if (window->isClosed()) break;
                if (smoke && frames == 1) ImGui::GetIO().AddKeyEvent(ImGuiKey_F5, true);
                if (smoke && frames == 2) ImGui::GetIO().AddKeyEvent(ImGuiKey_F5, false);
                if (physicsSmoke && frames == 2) editor.setShadowsEnabled(false);
                if (physicsSmoke && frames == 3) editor.setShadowsEnabled(true);
                if (physicsSmoke && frames == 240) ImGui::GetIO().AddKeyEvent(ImGuiKey_F5, true);
                if (physicsSmoke && frames == 241) ImGui::GetIO().AddKeyEvent(ImGuiKey_F5, false);
                if (physicsSmoke && frames == 244) editor.setShadowsEnabled(false);
                const bool capture=physicsSmoke && (frames==1 || frames==2 || frames==59 || frames==239 || frames==243 || frames==244);
                step();
                if (profilePhysics) {
                    if (frames >= 3 && !editor.playing()) throw std::runtime_error("Physics profile Play failed");
                    const auto& t = editor.frameTimings();
                    const std::array<double, 10> values{t.frameMs, t.uiMs, t.scriptMs, t.snapshotMs,
                        t.physicsApplyMs, t.physicsPrepareMs, t.physicsStepMs, t.renderSyncMs, t.renderMs, t.waitMs};
                    profile << frames << ',' << t.nodes << ',' << t.bodies;
                    for (const auto value : values) profile << ',' << value;
                    profile << '\n';
                    if (t.bodies >= 300 && t.bodies < 330) {
                        ++profileSamples;
                        for (std::size_t i = 0; i < values.size(); ++i) totals[i] += values[i];
                    }
                    if (t.bodies >= 330) break;
                }
                if(physicsSmoke && frames>=3 && frames<=240 && !editor.playing()) throw std::runtime_error("Physics smoke Play failed");
                if(physicsSmoke && frames>=242 && editor.playing()) throw std::runtime_error("Physics smoke Stop failed");
                if(capture) editor.captureViewport(captures/("frame-"+std::to_string(frames)+".ppm"));
                if (!profilePhysics) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (profilePhysics) {
                if (!profileSamples) throw std::runtime_error("Physics profile never reached 300 bodies");
                for (auto& value : totals) value /= profileSamples;
                std::printf("PROFILE 300-329 bodies (%u frames): frame %.3f, UI %.3f, scripts %.3f, snapshot %.3f, physics apply %.3f, prepare %.3f, step %.3f, render sync %.3f, render/present %.3f, wait %.3f ms\n",
                    profileSamples, totals[0], totals[1], totals[2], totals[3], totals[4], totals[5], totals[6], totals[7], totals[8], totals[9]);
            }
            if(physicsSmoke) {
                const auto read=[&](const char* name) {
                    std::ifstream file(captures/name,std::ios::binary);
                    return std::string(std::istreambuf_iterator<char>(file),{});
                };
                const auto initial=read("frame-2.ppm");
                if(initial==read("frame-3.ppm")) throw std::runtime_error("Shadow toggle did not affect the rendered scene");
                if(initial.empty() || initial==read("frame-240.ppm")) throw std::runtime_error("Physics did not move the rendered scene");
                // Shadow-camera fitting may move PCF edge samples despite identical scene poses.
                // Compare without shadows to require exact restoration of geometry and lighting.
                if(read("frame-3.ppm")!=read("frame-245.ppm")) throw std::runtime_error("Stop did not restore the rendered scene");
                if(read("frame-244.ppm")==read("frame-245.ppm")) throw std::runtime_error("Restored scene lost its shadows");
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
