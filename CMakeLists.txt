cmake_minimum_required(VERSION 3.25)
project(OgreThrone VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Compatibility for bundled dependency build scripts")
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

add_library(throne_audio STATIC src/audio/AudioSystem.cpp src/audio/SpatialSound.cpp)
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

add_library(throne_runtime STATIC src/runtime/PhysicsWorld.cpp)
target_include_directories(throne_runtime PUBLIC src)
target_link_libraries(throne_runtime PUBLIC box3d::box3d lhat)
target_compile_features(throne_runtime PUBLIC cxx_std_17)

add_executable(throne_smoke tests/runtime_smoke.cpp)
target_link_libraries(throne_smoke PRIVATE throne_runtime)
if(BUILD_TESTING)
    add_test(NAME runtime_smoke COMMAND throne_smoke)
endif()

if(THRONE_BUILD_GRAPHICS)
    add_executable(ogrethrone src/editor/main.cpp)
    target_link_libraries(ogrethrone PRIVATE throne_runtime throne_audio OgreNextMain
        RenderSystem_Vulkan OgreNextHlmsPbs OgreNextHlmsUnlit OgreNextImgui ColibriGui)
    target_include_directories(ogrethrone PRIVATE
        third_party/ogre-next/RenderSystems/Vulkan/include
        third_party/ogre-next/Components/Imgui/include
        third_party/imgui)
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT ogrethrone)
    throne_copy_audio_runtime(ogrethrone)
endif()
