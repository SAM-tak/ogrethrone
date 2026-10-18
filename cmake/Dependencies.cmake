foreach(dependency IN ITEMS ogre-next box3d imgui colibri rapidjson glslang)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/${dependency}/.git")
        message(FATAL_ERROR "Missing ${dependency}. Run: git submodule update --init --recursive")
    endif()
endforeach()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Use static dependencies")
add_subdirectory(third_party/box3d EXCLUDE_FROM_ALL)
# Box3D explicitly selects the static MSVC runtime; use the same runtime throughout.

foreach(feature IN ITEMS TESTS CLI LSP DAP BENCH)
    set(LHAT_BUILD_${feature} OFF CACHE BOOL "Only build the embedded L^ runtime")
endforeach()
# The standard library provides LTON reading/writing and the modules scripts may use.
set(LHAT_BUILD_STDLIB ON CACHE BOOL "Build the L^ standard library" FORCE)
add_subdirectory("${THRONE_LHAT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/third_party/lhat" EXCLUDE_FROM_ALL)

if(NOT THRONE_BUILD_GRAPHICS)
    return()
endif()

set(OGRE_SOURCE "${PROJECT_SOURCE_DIR}/third_party/ogre-next")
set(OGRE_BINARIES "${PROJECT_BINARY_DIR}/third_party/ogre-next")
set(OGRE_LIBRARIES OgreNextMain OgreNextHlmsUnlit)
set(OGRE_STATIC ON CACHE BOOL "Statically link OGRE")
set(OGRE_CONFIG_STATIC_LINK_CRT ON CACHE BOOL "Match the engine MSVC runtime")
set(OGRE_USE_NEW_PROJECT_NAME ON CACHE BOOL "Use OgreNext target names")
set(OGRE_SHADER_COMPILATION_THREADING_MODE 2 CACHE STRING "Enable threaded shader and PSO compilation")
set(OGRE_BUILD_RENDERSYSTEM_VULKAN ON CACHE BOOL "Vulkan is the engine graphics backend")
foreach(backend IN ITEMS D3D11 GL3PLUS GLES2 METAL)
    set(OGRE_BUILD_RENDERSYSTEM_${backend} OFF CACHE BOOL "Only build Vulkan")
endforeach()
foreach(feature IN ITEMS SAMPLES2 TOOLS TESTS COMPONENT_OVERLAY COMPONENT_PROPERTY COMPONENT_MESHLODGENERATOR)
    set(OGRE_BUILD_${feature} OFF CACHE BOOL "Disable unused OGRE targets")
endforeach()
set(OGRE_CONFIG_ENABLE_FREEIMAGE OFF CACHE BOOL "Use the built-in image codec")
set(OGRE_CONFIG_ENABLE_ZIP OFF CACHE BOOL "Load unpacked assets initially")
set(OGRE_CONFIG_ENABLE_JSON ON CACHE BOOL "Colibri skins and OGRE materials use RapidJSON" FORCE)
set(OGRE_CONFIG_ENABLE_STBI ON CACHE BOOL "Use the built-in image codec" FORCE)
set(Rapidjson_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/third_party/rapidjson/include")
set(OGRE_BUILD_COMPONENT_DEAR_IMGUI ON CACHE BOOL "Use OGRE's ImGui renderer")
set(OGRE_BUILD_COMPONENT_ATMOSPHERE ON CACHE BOOL "Use OGRE's procedural sky" FORCE)
# The upstream ImGui component concatenates this path with source filenames.
set(DearImgui_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/third_party/imgui/")
# OGRE uses glslang's C++ API directly. Build it with the same CRT and configuration.
foreach(feature IN ITEMS BUILD_EXTERNAL GLSLANG_TESTS GLSLANG_ENABLE_INSTALL
        ENABLE_OPT ENABLE_GLSLANG_BINARIES ENABLE_SPVREMAPPER)
    set(${feature} OFF CACHE BOOL "Only build the embedded GLSL compiler")
endforeach()
add_subdirectory(third_party/glslang EXCLUDE_FROM_ALL)
# Adapt OGRE's installed-header include path to glslang's source-tree layout.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/include/glslang/SPIRV")
file(WRITE "${CMAKE_BINARY_DIR}/include/glslang/SPIRV/GlslangToSpv.h"
    "#include \"${PROJECT_SOURCE_DIR}/third_party/glslang/SPIRV/GlslangToSpv.h\"\n")
set(Vulkan_SHADERC_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/third_party/glslang")
# OGRE's finder calls this shaderc, although this backend only needs glslang.
set(Vulkan_SHADERC_LIB_REL glslang CACHE STRING "Embedded GLSL compiler target" FORCE)
set(Vulkan_SHADERC_LIB_DBG glslang CACHE STRING "Embedded GLSL compiler target" FORCE)
foreach(package IN ITEMS ZZip FreeImage OpenGL DirectX11 AMDAGS OpenVR Remotery SDL2 RenderDoc)
    set(CMAKE_DISABLE_FIND_PACKAGE_${package} TRUE)
endforeach()

# Build Colibri's bundled text dependencies first, making FreeType available to OGRE.
set(COLIBRIGUI_LIB_ONLY ON CACHE BOOL "Build Colibri without its standalone demos")
set(COLIBRIGUI_USE_SYSTEM_LIBS OFF CACHE BOOL "Use the pinned bundled font stack")
add_subdirectory(third_party/colibri EXCLUDE_FROM_ALL)
target_include_directories(ColibriGui PUBLIC
    "${PROJECT_SOURCE_DIR}/third_party/colibri/include"
    "${PROJECT_SOURCE_DIR}/third_party/colibri/Dependencies/MSVC_Fix/2012")
target_include_directories(ColibriGui PRIVATE "${PROJECT_SOURCE_DIR}/third_party/rapidjson/include")

add_subdirectory(third_party/ogre-next EXCLUDE_FROM_ALL)
# Upstream's unity-exclusion list re-adds this codec even when FreeImage is off.
if(NOT OGRE_CONFIG_ENABLE_FREEIMAGE)
    get_target_property(ogre_core_sources OgreNextMain SOURCES)
    list(FILTER ogre_core_sources EXCLUDE REGEX "(^|/)OgreFreeImageCodec2[.]cpp$")
    set_property(TARGET OgreNextMain PROPERTY SOURCES "${ogre_core_sources}")
endif()
if(NOT TARGET RenderSystem_Vulkan)
    message(FATAL_ERROR
        "OGRE requires Vulkan headers and loader. Install the Vulkan SDK, "
        "then set OGRE_VULKAN_SDK or VULKAN_SDK. For CPU checks use the headless preset.")
endif()
if(NOT TARGET OgreNextImgui)
    message(FATAL_ERROR "OGRE ImGui integration was not configured")
endif()
target_include_directories(OgreNextMain INTERFACE
    "${OGRE_SOURCE}/OgreMain/include" "${OGRE_BINARIES}/include")
target_include_directories(RenderSystem_Vulkan INTERFACE "${Vulkan_INCLUDE_DIR}")
target_include_directories(RenderSystem_Vulkan BEFORE PRIVATE
    "${PROJECT_SOURCE_DIR}/third_party/glslang" "${CMAKE_BINARY_DIR}/include")
if(WIN32)
    target_include_directories(OgreNextMain INTERFACE "${OGRE_SOURCE}/OgreMain/include/WIN32")
endif()
target_link_libraries(ColibriGui OgreNextMain OgreNextHlmsUnlit)
target_include_directories(OgreNextImgui PUBLIC "${PROJECT_SOURCE_DIR}/third_party/imgui")
# The renderer expects pointer texture IDs, matching ImGui versions before 1.91.4.
target_compile_definitions(OgreNextImgui PUBLIC "ImTextureID=void*")
