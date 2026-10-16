if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/soloud/include/soloud.h")
    message(FATAL_ERROR "Missing SoLoud. Run: git submodule update --init --recursive")
endif()

# Build only the mixer, PCM/file sources and our two output backends.
set(soloud_source "${PROJECT_SOURCE_DIR}/third_party/soloud")
file(GLOB soloud_core CONFIGURE_DEPENDS "${soloud_source}/src/core/*.cpp")
file(GLOB soloud_wav CONFIGURE_DEPENDS "${soloud_source}/src/audiosource/wav/*.cpp")
add_library(throne_soloud STATIC ${soloud_core} ${soloud_wav}
    "${soloud_source}/src/audiosource/wav/stb_vorbis.c"
    "${soloud_source}/src/backend/miniaudio/soloud_miniaudio.cpp"
    "${soloud_source}/src/backend/null/soloud_null.cpp")
target_include_directories(throne_soloud PUBLIC "${soloud_source}/include")
target_compile_definitions(throne_soloud PRIVATE WITH_MINIAUDIO WITH_NULL
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
find_package(Threads REQUIRED)
target_link_libraries(throne_soloud PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(throne_soloud PRIVATE winmm ole32)
elseif(APPLE)
    target_link_libraries(throne_soloud PRIVATE "-framework CoreAudio" "-framework AudioToolbox"
        "-framework CoreFoundation")
endif()

# The official C SDK avoids rebuilding Steam Audio's separate toolchain/dependencies.
set(THRONE_STEAM_AUDIO_ROOT "" CACHE PATH "Existing Steam Audio 4.8.1 SDK directory")
if(NOT THRONE_STEAM_AUDIO_ROOT)
    include(FetchContent)
    FetchContent_Declare(throne_steam_audio_sdk
        URL https://github.com/ValveSoftware/steam-audio/releases/download/v4.8.1/steamaudio_4.8.1.zip
        URL_HASH SHA256=4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(throne_steam_audio_sdk)
    set(THRONE_STEAM_AUDIO_ROOT "${throne_steam_audio_sdk_SOURCE_DIR}")
endif()
if(NOT EXISTS "${THRONE_STEAM_AUDIO_ROOT}/include/phonon.h")
    message(FATAL_ERROR "THRONE_STEAM_AUDIO_ROOT must contain include/phonon.h")
endif()

add_library(throne_steam_audio SHARED IMPORTED GLOBAL)
set_target_properties(throne_steam_audio PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${THRONE_STEAM_AUDIO_ROOT}/include")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(steam_audio_lib "${THRONE_STEAM_AUDIO_ROOT}/lib/windows-x64")
    set_target_properties(throne_steam_audio PROPERTIES
        IMPORTED_IMPLIB "${steam_audio_lib}/phonon.lib"
        IMPORTED_LOCATION "${steam_audio_lib}/phonon.dll")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set_target_properties(throne_steam_audio PROPERTIES
        IMPORTED_LOCATION "${THRONE_STEAM_AUDIO_ROOT}/lib/linux-x64/libphonon.so")
elseif(APPLE AND NOT IOS)
    set_target_properties(throne_steam_audio PROPERTIES
        IMPORTED_LOCATION "${THRONE_STEAM_AUDIO_ROOT}/lib/osx/libphonon.dylib")
else()
    message(FATAL_ERROR "Steam Audio SDK mapping is currently configured for desktop x64 and macOS")
endif()

function(throne_copy_audio_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:throne_steam_audio>" "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/audio-licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/licenses/steam-audio-LICENSE.md"
            "$<TARGET_FILE_DIR:${target}>/audio-licenses/steam-audio-LICENSE.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${THRONE_STEAM_AUDIO_ROOT}/THIRDPARTY.md"
            "$<TARGET_FILE_DIR:${target}>/audio-licenses/steam-audio-THIRDPARTY.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/third_party/soloud/LICENSE"
            "$<TARGET_FILE_DIR:${target}>/audio-licenses/soloud-LICENSE.txt"
        VERBATIM)
endfunction()
