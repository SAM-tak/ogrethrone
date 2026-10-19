cmake_minimum_required(VERSION 3.25)
project(OgreThrone VERSION 0.1.0 LANGUAGES C CXX)

# Keep bundled dependency policies above CMake's deprecated compatibility range.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(THRONE_BUILD_GRAPHICS "Build the Vulkan graphics bootstrap and UI libraries" ON)
set(THRONE_LHAT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lhat" CACHE PATH
    "L^ source directory; may point to a sibling development checkout")
include(CTest)
include(cmake/Dependencies.cmake)
include(cmake/Audio.cmake)

# Headers are listed too, so that IDEs (CMake Tools) know which target's settings parse them.
add_library(throne_audio STATIC src/audio/AudioSystem.cpp src/audio/SpatialSound.cpp
    src/audio/AudioSystem.h src/audio/SpatialSound.h src/audio/SteamAudioMath.h)
target_include_directories(throne_audio PUBLIC src)
target_link_libraries(throne_audio PUBLIC throne_soloud throne_steam_audio)

foreach(check IN ITEMS audio acoustics)
    add_executable(throne_${check}_smoke tests/${check}_smoke.cpp)
    target_link_libraries(throne_${check}_smoke PRIVATE throne_audio)
    target_compile_definitions(throne_${check}_smoke PRIVATE
        THRONE_AUDIO_FIXTURES="${PROJECT_SOURCE_DIR}/tests/audio")
    throne_copy_audio_runtime(throne_${check}_smoke)
    if(BUILD_TESTING)
        add_test(NAME ${check}_smoke COMMAND throne_${check}_smoke)
        set_tests_properties(${check}_smoke PROPERTIES TIMEOUT 30)
    endif()
endforeach()

add_library(throne_runtime STATIC src/runtime/PhysicsWorld.cpp src/runtime/PhysicsWorld.h src/runtime/Box3DMath.h)
target_include_directories(throne_runtime PUBLIC src)
target_link_libraries(throne_runtime PUBLIC box3d::box3d lhat)
target_compile_features(throne_runtime PUBLIC cxx_std_17)

add_executable(throne_smoke tests/runtime_smoke.cpp)
target_link_libraries(throne_smoke PRIVATE throne_runtime)
target_compile_definitions(throne_smoke PRIVATE THRONE_PHYSICS_SAMPLE="${PROJECT_SOURCE_DIR}/samples/physics")
if(BUILD_TESTING)
    add_test(NAME runtime_smoke COMMAND throne_smoke)
endif()

# Levels, projects and the project list: the editor's LTON data, without OGRE.
add_library(throne_data STATIC src/editor/Lton.cpp src/editor/Scene.cpp src/editor/Level.cpp
    src/editor/Expansion.cpp src/editor/Project.cpp src/editor/PrimitiveGeometry.cpp src/editor/PrimitiveGeometry.h src/runtime/FramePipeline.cpp src/runtime/FramePipeline.h src/editor/Lton.h src/editor/Scene.h src/editor/Level.h
    src/editor/Expansion.h src/editor/Project.h src/core/Math.h src/core/Ids.h)
target_include_directories(throne_data PUBLIC src)
# stdlib/*.h are included by path relative to the L^ source root.
target_include_directories(throne_data PRIVATE "${THRONE_LHAT_SOURCE_DIR}")
target_link_libraries(throne_data PUBLIC lhat PRIVATE lhatstdlib)
target_link_libraries(throne_runtime PUBLIC throne_data)

add_executable(throne_data_smoke tests/data_smoke.cpp)
target_link_libraries(throne_data_smoke PRIVATE throne_data)
if(BUILD_TESTING)
    add_test(NAME data_smoke COMMAND throne_data_smoke)
endif()

add_executable(throne_showcase_data_smoke tests/showcase_data_smoke.cpp)
target_link_libraries(throne_showcase_data_smoke PRIVATE throne_data)
if(BUILD_TESTING)
    add_test(NAME showcase_data_smoke COMMAND throne_showcase_data_smoke "${PROJECT_SOURCE_DIR}/samples/showcase")
endif()

add_library(throne_script STATIC src/script/ScriptRuntime.cpp src/script/ScriptRuntime.h)
target_include_directories(throne_script PRIVATE "${THRONE_LHAT_SOURCE_DIR}")
target_link_libraries(throne_script PUBLIC throne_data PRIVATE lhatstdlib)
target_link_libraries(throne_smoke PRIVATE throne_script)

add_executable(throne_script_smoke tests/script_smoke.cpp)
target_link_libraries(throne_script_smoke PRIVATE throne_script)
if(BUILD_TESTING)
    add_test(NAME script_smoke COMMAND throne_script_smoke "${PROJECT_SOURCE_DIR}/samples/basic")
endif()

add_executable(throne_ecs_smoke tests/ecs_smoke.cpp)
add_executable(throne_behaviour_system_smoke tests/behaviour_system_smoke.cpp)
target_link_libraries(throne_behaviour_system_smoke PRIVATE throne_script)
target_link_libraries(throne_ecs_smoke PRIVATE throne_script)
if(BUILD_TESTING)
    add_test(NAME ecs_smoke COMMAND throne_ecs_smoke)
    add_test(NAME behaviour_system_smoke COMMAND throne_behaviour_system_smoke)
    set_tests_properties(ecs_smoke PROPERTIES TIMEOUT 30)
endif()

if(THRONE_BUILD_GRAPHICS)
    # ImGuizmo is compiled here against the ImGui core that OgreNextImgui builds.
    add_executable(ogrethrone src/editor/main.cpp src/editor/Editor.cpp src/editor/EditorPanels.cpp
        src/editor/EditorPlatform.cpp src/editor/EditorParticles.cpp src/editor/References.cpp src/editor/Model.cpp src/editor/Environment.cpp src/editor/GlobalIllumination.cpp src/editor/Tasks.cpp
        third_party/imguizmo/src/ImGuizmo.cpp third_party/ufbx/ufbx.c
        src/editor/Editor.h src/editor/EditorPlatform.h src/editor/OgreConversions.h src/editor/References.h
        src/editor/Model.h src/editor/Environment.h src/editor/GlobalIllumination.h src/editor/Tasks.h)
    # ufbx reads FBX for the Model component; its warnings are its own.
    set_source_files_properties(third_party/ufbx/ufbx.c PROPERTIES COMPILE_OPTIONS "$<IF:$<BOOL:${MSVC}>,/w,-w>")
    if(WIN32)
        # The application icon and the manifest (visual styles, per-monitor DPI, UTF-8).
        target_sources(ogrethrone PRIVATE media/ogrethrone.rc media/ogrethrone.manifest)
        target_link_libraries(ogrethrone PRIVATE dwmapi ole32 shell32 uuid imm32)
    endif()
    target_link_libraries(ogrethrone PRIVATE throne_runtime throne_audio throne_script OgreNextMain
        RenderSystem_Vulkan OgreNextHlmsPbs OgreNextHlmsUnlit OgreNextImgui OgreNextAtmosphere Plugin_ParticleFX2 ColibriGui)
    target_include_directories(ogrethrone PRIVATE
        third_party/ogre-next/RenderSystems/Vulkan/include
        third_party/ogre-next/Components/Imgui/include
        third_party/ufbx
        third_party/ogre-next/Components/Hlms/Common/include
        third_party/ogre-next/Components/Hlms/Unlit/include
        third_party/ogre-next/Components/Hlms/Pbs/include
        third_party/ogre-next/PlugIns/ParticleFX2/include
        third_party/ogre-next/Components/Atmosphere/include
        third_party/imgui
        third_party/imguizmo/src)
    # Hlms shader templates are read from the pinned OGRE checkout until asset packaging exists.
    target_compile_definitions(ogrethrone PRIVATE
        THRONE_OGRE_MEDIA_DIR="${PROJECT_SOURCE_DIR}/third_party/ogre-next/Samples/Media"
        THRONE_MEDIA_DIR="${PROJECT_SOURCE_DIR}/media")
    if(WIN32)
        # The Win32 platform backend links against the ImGui core built by OgreNextImgui.
        target_sources(ogrethrone PRIVATE third_party/imgui/backends/imgui_impl_win32.cpp)
        target_link_libraries(ogrethrone PRIVATE comctl32)
    endif()
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT ogrethrone)
    throne_copy_audio_runtime(ogrethrone)
    if(BUILD_TESTING)
        target_sources(ogrethrone PRIVATE tests/navigation_smoke.cpp tests/navigation_smoke.h tests/showcase_smoke.cpp tests/showcase_smoke.h)
        target_include_directories(ogrethrone PRIVATE tests)
        target_compile_definitions(ogrethrone PRIVATE THRONE_NAVIGATION_SMOKE)
    endif()
endif()
