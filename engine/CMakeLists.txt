set(ENGINE_WEB_BUILD FALSE)

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR EMSCRIPTEN)
    set(ENGINE_WEB_BUILD TRUE)
endif()

message(STATUS "ENGINE_WEB_BUILD=${ENGINE_WEB_BUILD}")
message(STATUS "ENGINE_CORE_ONLY=${ENGINE_CORE_ONLY}")

# Shared engine libraries, so an editor and a game module it loads at runtime
# share one copy of the engine's state (component ids, logger, the active Lua
# VM, and the SDL / ImGui instances linked into it). The web build links one
# wasm module and stays static.
#
# Not with MSVC by default: a DLL there exports only what's marked
# __declspec(dllexport), and the engine marks nothing (GCC/Clang and MinGW
# export everything), so an MSVC DLL build has no import libraries to link.
# MSVC builds are static; the editor's Windows build uses MinGW.
if(MSVC)
    set(ENGINE_SHARED_DEFAULT OFF)
else()
    set(ENGINE_SHARED_DEFAULT ON)
endif()
option(ENGINE_SHARED "Build the engine libraries as shared libraries" ${ENGINE_SHARED_DEFAULT})
if(ENGINE_SHARED AND NOT ENGINE_WEB_BUILD)
    set(ENGINE_LIBRARY_TYPE SHARED)
else()
    set(ENGINE_LIBRARY_TYPE STATIC)
endif()
message(STATUS "ENGINE_LIBRARY_TYPE=${ENGINE_LIBRARY_TYPE}")

# sfs-codegen generates reflection from headers. It must run on the build
# machine, so a native build builds it, while a cross-build (e.g. Windows from
# WSL) uses a host copy given in SFS_CODEGEN_EXECUTABLE. Core-only and web
# builds have no codegen (no tree-sitter), and register no engine types.
set(SFS_CODEGEN_EXECUTABLE "" CACHE FILEPATH
    "Host sfs-codegen to use when cross-compiling (e.g. from a native build's bin/)")
if(ENGINE_CORE_ONLY OR ENGINE_WEB_BUILD)
    set(SFS_CODEGEN_AVAILABLE FALSE)
elseif(CMAKE_CROSSCOMPILING)
    if(NOT SFS_CODEGEN_EXECUTABLE)
        message(FATAL_ERROR
            "Cross-compiling needs a host sfs-codegen: build the engine natively "
            "once, then pass -DSFS_CODEGEN_EXECUTABLE=<native-build>/bin/sfs-codegen")
    endif()
    set(SFS_CODEGEN_COMMAND "${SFS_CODEGEN_EXECUTABLE}")
    set(SFS_CODEGEN_DEPENDS "${SFS_CODEGEN_EXECUTABLE}")
    set(SFS_CODEGEN_AVAILABLE TRUE)
else()
    add_subdirectory(tools/codegen)
    set(SFS_CODEGEN_COMMAND sfs-codegen)
    set(SFS_CODEGEN_DEPENDS sfs-codegen)
    set(SFS_CODEGEN_AVAILABLE TRUE)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sfsCodegen.cmake")

# Engine DLLs go next to the executables (Windows finds DLLs beside the exe,
# not via an rpath).
function(sfs_engine_library_output target)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endfunction()

# AddressSanitizer + UndefinedBehaviorSanitizer build, used by the sanitize CI
# job. The flags apply to the engine libraries and the tests, which are the only
# targets defined under this directory; Conan dependencies are prebuilt and stay
# uninstrumented.
option(ENGINE_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(ENGINE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Standard install layout (CMAKE_INSTALL_INCLUDEDIR / LIBDIR), used both by the
# INSTALL_INTERFACE include paths below and by the install/export section.
include(GNUInstallDirs)

# ----------------------------------------------------------------------------
# engine-core: ECS, scripting, and plain-data subsystems. No SDL/GL/imgui and no
# Conan dependencies -- it builds against vendored Lua + glm alone, so a consumer
# (e.g. the test suite) can link it without the desktop/render stack.
# ----------------------------------------------------------------------------

file(GLOB_RECURSE ENGINE_CORE_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
)

# Vendored Lua 5.4 (minus the standalone interpreter/compiler mains), compiled
# into core on every platform so scripting needs no external Lua.
file(GLOB LUA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/lib/lua/*.c")
list(FILTER LUA_SRC EXCLUDE REGEX ".*/lua\\.c$")
list(FILTER LUA_SRC EXCLUDE REGEX ".*/luac\\.c$")

add_library(engine-core ${ENGINE_LIBRARY_TYPE} ${ENGINE_CORE_SRC} ${LUA_SRC})
sfs_engine_library_output(engine-core)

# [[sfs::...]] attributes are editor annotations read by sfs-codegen; compilers
# should ignore them, for engine and game code alike (hence PUBLIC).
target_compile_options(engine-core PUBLIC
    $<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=sfs::>
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>
)

# Reflection for the engine's own components, generated the same way a game's
# is. Without codegen (core-only / web) a stub registers nothing.
if(SFS_CODEGEN_AVAILABLE)
    set(ENGINE_COMPONENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include/engine/core/components")
    sfs_generate_reflection(engine-core
        MODULE engine
        FUNCTION sfs::reflect::registerEngineTypes
        INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/include"
        HEADERS
            "${ENGINE_COMPONENT_DIR}/nameComponent.h"
            "${ENGINE_COMPONENT_DIR}/transform3DComponent.h"
            "${ENGINE_COMPONENT_DIR}/meshRendererComponent.h"
            "${ENGINE_COMPONENT_DIR}/pointLight3DComponent.h"
            "${ENGINE_COMPONENT_DIR}/camera3DComponent.h"
            "${ENGINE_COMPONENT_DIR}/rigidBody3DComponent.h"
            "${ENGINE_COMPONENT_DIR}/boxCollider3DComponent.h"
            "${ENGINE_COMPONENT_DIR}/sphereCollider3DComponent.h"
            "${ENGINE_COMPONENT_DIR}/capsuleCollider3DComponent.h"
            "${ENGINE_COMPONENT_DIR}/scriptComponent.h")
else()
    target_sources(engine-core PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/stubs/noEngineReflection.cpp")
endif()

# Allocation tracking replaces the global operator new, which is only sound when
# the engine and the executable are one binary (see allocationMetrics.cpp).
if(ENGINE_LIBRARY_TYPE STREQUAL "STATIC")
    target_compile_definitions(engine-core PRIVATE SFS_TRACK_ALLOCATIONS=1)
else()
    target_compile_definitions(engine-core PRIVATE SFS_TRACK_ALLOCATIONS=0)
endif()

# Lua's POSIX build uses mkstemp for temp files instead of the unsafe tmpnam.
if(UNIX AND NOT ENGINE_WEB_BUILD)
    set_source_files_properties(${LUA_SRC} PROPERTIES COMPILE_DEFINITIONS LUA_USE_POSIX)
endif()

# MinGW builds link the C++ runtime statically into each binary (see the
# windows-mingw Conan profile), but the posix-threads runtime still needs the
# shared winpthread DLL. Ship it next to the executables.
if(MINGW)
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libwinpthread-1.dll
        OUTPUT_VARIABLE winpthreadDll
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(EXISTS "${winpthreadDll}")
        file(COPY "${winpthreadDll}" DESTINATION "${CMAKE_BINARY_DIR}/bin")
    else()
        message(WARNING "libwinpthread-1.dll not found; executables may not start")
    endif()
endif()

if(ENGINE_SANITIZE)
    # FastNoiseLite's hash multiply relies on signed integer overflow wrapping,
    # defined by convention but flagged by UBSan. noise.cpp is the only unit that
    # includes it, so drop just that one check for just this file.
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/noise/noise.cpp"
        PROPERTIES COMPILE_OPTIONS -fno-sanitize=signed-integer-overflow)
    # Jolt is built without RTTI, so its classes have no typeinfo for UBSan's
    # vptr check to look up (undefined references at link time).
    # physicsSystem3D.cpp is the only unit that includes Jolt: drop that one
    # check for just this file. (Appended: it has other options of its own.)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/systems/physicsSystem3D.cpp"
        APPEND PROPERTY COMPILE_OPTIONS -fno-sanitize=vptr)
endif()

# Namespaced alias so in-tree consumers (sample game, web build) link the exact
# same target name an out-of-tree consumer gets from find_package(engine).
add_library(sfs::engine-core ALIAS engine-core)

# Public include roots use BUILD/INSTALL_INTERFACE so the exported target is
# relocatable: in-tree builds see the source dirs; installed consumers see the
# headers staged under the install prefix (see the install section below).
target_include_directories(engine-core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/glm>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/lib>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/lib/glm>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/lua"
)

target_compile_options(engine-core PRIVATE
    # -Wall is GCC/Clang syntax; MSVC's /Wall is a different, far noisier set.
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
    # Clang-only (silences unknown -Wno-* flags); GCC doesn't recognise it.
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-warning-option>
    # MSVC caps a single object at 64k sections; the template-heavy units exceed
    # it without /bigobj.
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
)

if(ENGINE_WEB_BUILD)
    target_compile_definitions(engine-core PUBLIC ENGINE_WEB=1)
    # Threads/atomics flags must match across every wasm object in the link.
    target_compile_options(engine-core PUBLIC -pthread -matomics -mbulk-memory)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(engine-core PUBLIC Threads::Threads)
endif()

# ----------------------------------------------------------------------------
# engine: the render/platform layer (SDL, OpenGL, imgui). Depends on engine-core.
# Skipped entirely by a core-only configure (e.g. the CI test job): no render
# lib, no SDL, nothing that needs Conan.
# ----------------------------------------------------------------------------
if(NOT ENGINE_CORE_ONLY)

file(GLOB_RECURSE ENGINE_RENDER_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/*.cpp"
)
# 3D physics is built on Jolt, which the web build doesn't provide.
if(ENGINE_WEB_BUILD)
    list(FILTER ENGINE_RENDER_SRC EXCLUDE REGEX "/src/runtime/systems/physicsSystem3D\\.cpp$")
endif()

file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

add_library(engine ${ENGINE_LIBRARY_TYPE}
    ${ENGINE_RENDER_SRC}
    ${ENGINE_HEADERS}
)

add_library(sfs::engine ALIAS engine)
sfs_engine_library_output(engine)

target_link_libraries(engine PUBLIC engine-core)

target_include_directories(engine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_options(engine PRIVATE
    # -Wall is GCC/Clang syntax; MSVC's /Wall is a different, far noisier set.
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
    # Clang-only (silences unknown -Wno-* flags); GCC doesn't recognise it.
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-warning-option>
    # MSVC caps a single object at 64k sections; the big GL render units
    # (openGLQuadRenderer, isometricGeometryRenderer) exceed it without /bigobj.
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
)

# Embed the GLSL shaders. The renderers' shaders live in real .vert/.frag files
# under src/.../gl/shaders; embedShaders.cmake bakes them into a generated header
# (byte arrays -> no runtime file loading, and no string-literal-size limit). The
# renderers include <engine/generated/embeddedShaders.h>. Regenerated whenever a
# shader file changes (custom-command DEPENDS); CONFIGURE_DEPENDS picks up
# adding/removing one.
set(ENGINE_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/rendering/gl/shaders")
file(GLOB_RECURSE ENGINE_SHADER_FILES CONFIGURE_DEPENDS
    "${ENGINE_SHADER_DIR}/*.vert" "${ENGINE_SHADER_DIR}/*.frag")
set(ENGINE_SHADERS_HEADER
    "${CMAKE_CURRENT_BINARY_DIR}/generated/engine/generated/embeddedShaders.h")
add_custom_command(
    OUTPUT "${ENGINE_SHADERS_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        "-DSHADER_DIR=${ENGINE_SHADER_DIR}"
        "-DOUT=${ENGINE_SHADERS_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embedShaders.cmake"
    DEPENDS ${ENGINE_SHADER_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embedShaders.cmake"
    COMMENT "Embedding GLSL shaders"
    VERBATIM)
# custom_target + add_dependencies guarantees the header is generated before the
# engine compiles on every generator (including the multi-config VS build).
add_custom_target(engineShaders DEPENDS "${ENGINE_SHADERS_HEADER}")
add_dependencies(engine engineShaders)
target_sources(engine PRIVATE "${ENGINE_SHADERS_HEADER}")
target_include_directories(engine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/include/assets
     DESTINATION ${CMAKE_BINARY_DIR}/bin)

if(NOT ENGINE_WEB_BUILD AND CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_include_directories(engine PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/tracy/public>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/lib/tracy/public>"
    )

    target_sources(engine PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/tracy/public/TracyClient.cpp"
    )

    target_compile_definitions(engine PUBLIC
        TRACY_ENABLE
    )
endif()

if(ENGINE_WEB_BUILD)
    target_compile_options(engine PUBLIC
        -sUSE_SDL=2
        -sUSE_SDL_IMAGE=2
        -sSDL2_IMAGE_FORMATS=["png"]
        -sUSE_SDL_TTF=2
        -sUSE_SDL_MIXER=2
    )

    target_link_options(engine PUBLIC
        -sUSE_SDL=2
        -sUSE_SDL_IMAGE=2
        -sSDL2_IMAGE_FORMATS=["png"]
        -sUSE_SDL_TTF=2
        -sUSE_SDL_MIXER=2
        -sMIN_WEBGL_VERSION=2
        -sMAX_WEBGL_VERSION=2
        -sFULL_ES3=1
        -sUSE_PTHREADS=1
        -sPTHREAD_POOL_SIZE=4
    )
else()
    find_package(SDL2 REQUIRED)
    find_package(SDL2_image REQUIRED)
    find_package(SDL2_ttf REQUIRED)
    find_package(SDL2_mixer REQUIRED)
    find_package(imgui REQUIRED)
    find_package(OpenGL REQUIRED)
    find_package(GLEW REQUIRED)
    find_package(Jolt REQUIRED)
    find_package(cgltf REQUIRED)

    # Static and private: Jolt (physics) and cgltf (model import) stay out of
    # the engine's headers, so games and the SDK never see them. The web build
    # has neither, and leaves those features out.
    target_link_libraries(engine PRIVATE Jolt::Jolt cgltf::cgltf)
    target_compile_definitions(engine PRIVATE SFS_HAS_PHYSICS=1 SFS_HAS_MODEL_IMPORT=1)
    # Jolt's package is built for AVX2 (its defines say so) but doesn't pass on
    # the matching compiler flags; the code including its headers needs them.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        # (Appended, not set: the sanitizer build adds an option of its own.)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/systems/physicsSystem3D.cpp"
            APPEND PROPERTY COMPILE_OPTIONS -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma)
    endif()

    target_include_directories(engine PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/lib/imgui>"
    )

    target_sources(engine PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_sdl2.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_opengl3.cpp"
    )

    # The backends compile into the engine, not into the (shared) imgui
    # library, so they must not inherit its dllimport linkage (IMGUI_IMPL_API
    # defaults to IMGUI_API). Public, because engine-webgpu calls them too.
    target_compile_definitions(engine PUBLIC "IMGUI_IMPL_API=")

    target_link_libraries(engine
        PUBLIC
            SDL2::SDL2
            SDL2_image::SDL2_image
            imgui::imgui
            sdl_ttf::sdl_ttf
            SDL2_mixer::SDL2_mixer
            OpenGL::GL
            GLEW::GLEW
    )
endif()

# ----------------------------------------------------------------------------
# engine-webgpu: the WebGPU render backend + GPU-voxel subsystem (brickmap +
# raymarch + GPU water CA). A separate lib so only a game that wants it
# (sampleVoxel3D) links wgpu-native; the OpenGL iso games link plain `engine`
# and never pull in WebGPU. Native only (wgpu-native has no web target here).
# ----------------------------------------------------------------------------
if(NOT ENGINE_WEB_BUILD)
    include(FetchContent)

    # WebGPU via wgpu-native (prebuilt binaries; pinned tag, not a branch tip).
    FetchContent_Declare(webgpu
        GIT_REPOSITORY https://github.com/eliemichel/WebGPU-distribution.git
        GIT_TAG wgpu-v24.0.0.2
    )
    FetchContent_MakeAvailable(webgpu)

    # The SDL2 -> WGPUSurface helper.
    FetchContent_Declare(sdl2webgpu
        GIT_REPOSITORY https://github.com/eliemichel/sdl2webgpu.git
        GIT_TAG eda8bedcb78a755f42c5efa00b732c62e89c23cb # main @ 2024, v24-compatible
    )
    FetchContent_MakeAvailable(sdl2webgpu)
    # A static library linked into the shared engine-webgpu, so it must be
    # position-independent (a sanitizer build's instrumentation can't link
    # otherwise).
    set_target_properties(sdl2webgpu PROPERTIES POSITION_INDEPENDENT_CODE ON)

    file(GLOB_RECURSE ENGINE_WEBGPU_SRC CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/webgpu/*.cpp")

    # Dear ImGui's WebGPU renderer backend (the SDL2 platform backend is already
    # in `engine`). Multi-viewport rendering, which it lacks, lives in
    # src/webgpu/imgui.
    list(APPEND ENGINE_WEBGPU_SRC
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_wgpu.cpp")

    add_library(engine-webgpu ${ENGINE_LIBRARY_TYPE} ${ENGINE_WEBGPU_SRC})
    sfs_engine_library_output(engine-webgpu)

    # Select the wgpu-native flavour of the backend's WebGPU API.
    target_compile_definitions(engine-webgpu PRIVATE IMGUI_IMPL_WEBGPU_BACKEND_WGPU)
    add_library(sfs::engine-webgpu ALIAS engine-webgpu)

    # sdl2webgpu is only used inside the library (surface creation), so it
    # stays private; wgpu-native is public, as the headers expose its API.
    target_link_libraries(engine-webgpu PUBLIC engine webgpu PRIVATE sdl2webgpu)

    # Stage the engine's runtime libraries next to an executable: wgpu-native
    # (engine DLLs already build into the shared bin/). The installed package
    # defines the same function (engineConfig.cmake), so a consumer calls it
    # identically in-tree and against an install.
    function(sfs_deploy_runtime target)
        target_copy_webgpu_binaries(${target})
    endfunction()

    target_include_directories(engine-webgpu PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_compile_options(engine-webgpu PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
        $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-warning-option>
        $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
    )

    # Embed the WGSL shaders. Same mechanism as the GLSL embed above: real .wgsl
    # files -> a generated byte-array header, included as
    # <engine/generated/embeddedWgsl.h> (voxelWater.wgsl -> sfs::shaders::voxelWaterWgsl).
    set(ENGINE_WGSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/webgpu/shaders")
    file(GLOB_RECURSE ENGINE_WGSL_FILES CONFIGURE_DEPENDS "${ENGINE_WGSL_DIR}/*.wgsl")
    set(ENGINE_WGSL_HEADER
        "${CMAKE_CURRENT_BINARY_DIR}/generated/engine/generated/embeddedWgsl.h")
    add_custom_command(
        OUTPUT "${ENGINE_WGSL_HEADER}"
        COMMAND ${CMAKE_COMMAND}
            "-DSHADER_DIR=${ENGINE_WGSL_DIR}"
            "-DOUT=${ENGINE_WGSL_HEADER}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embedShaders.cmake"
        DEPENDS ${ENGINE_WGSL_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embedShaders.cmake"
        COMMENT "Embedding WGSL shaders"
        VERBATIM)
    add_custom_target(engineWgslShaders DEPENDS "${ENGINE_WGSL_HEADER}")
    add_dependencies(engine-webgpu engineWgslShaders)
    target_sources(engine-webgpu PRIVATE "${ENGINE_WGSL_HEADER}")
    target_include_directories(engine-webgpu PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()

endif() # NOT ENGINE_CORE_ONLY

# ----------------------------------------------------------------------------
# Install / export. Lets a consumer `find_package(engine)` against an installed
# tree instead of vendoring this source (or pulling it in via add_subdirectory).
# Always exports engine-core; also exports the render `engine` lib when it was
# built. Skipped on the web toolchain, where an install has no meaning.
# ----------------------------------------------------------------------------
if(NOT ENGINE_WEB_BUILD)
    include(CMakePackageConfigHelpers)

    option(ENGINE_INSTALL_CMAKE_PACKAGE
        "Generate + install the CMake package config (engineConfig.cmake)" ON)

    set(ENGINE_INSTALL_TARGETS engine-core)
    if(TARGET engine)
        list(APPEND ENGINE_INSTALL_TARGETS engine)
        set(ENGINE_HAS_RENDER_LIB TRUE)
    else()
        set(ENGINE_HAS_RENDER_LIB FALSE)
    endif()

    # engine-webgpu, plus the prebuilt wgpu-native library + headers, which
    # the installed config re-creates as the `webgpu` imported target. Only
    # the shared build installs it: sdl2webgpu is linked privately, which a
    # shared library hides, but a static one would have to export (and its
    # include directory points into the build tree).
    if(TARGET engine-webgpu AND ENGINE_LIBRARY_TYPE STREQUAL "STATIC")
        message(STATUS "engine-webgpu is not installed in a static engine build")
    endif()
    if(TARGET engine-webgpu AND ENGINE_LIBRARY_TYPE STREQUAL "SHARED")
        list(APPEND ENGINE_INSTALL_TARGETS engine-webgpu)
        set(ENGINE_HAS_WEBGPU_LIB TRUE)

        get_target_property(wgpuRuntimeLib webgpu IMPORTED_LOCATION)
        get_filename_component(wgpuRuntimeName "${wgpuRuntimeLib}" NAME)
        get_filename_component(wgpuLibDir "${wgpuRuntimeLib}" DIRECTORY)
        get_filename_component(wgpuRoot "${wgpuLibDir}" DIRECTORY)
        set(ENGINE_WGPU_RUNTIME_NAME "${wgpuRuntimeName}")

        if(WIN32)
            install(FILES "${wgpuRuntimeLib}" DESTINATION ${CMAKE_INSTALL_BINDIR})
            get_target_property(wgpuImportLib webgpu IMPORTED_IMPLIB)
            get_filename_component(ENGINE_WGPU_IMPLIB_NAME "${wgpuImportLib}" NAME)
            install(FILES "${wgpuImportLib}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
        else()
            install(FILES "${wgpuRuntimeLib}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
        endif()

        install(DIRECTORY "${wgpuRoot}/include/"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/wgpu-native)
        # WebGPU-distribution's webgpu.hpp wrapper and its include root.
        get_target_property(wgpuIncludeDirs webgpu INTERFACE_INCLUDE_DIRECTORIES)
        list(GET wgpuIncludeDirs 0 wgpuDistributionInclude)
        install(DIRECTORY "${wgpuDistributionInclude}/"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/wgpu-native)
    else()
        set(ENGINE_HAS_WEBGPU_LIB FALSE)
    endif()

    install(TARGETS ${ENGINE_INSTALL_TARGETS}
        EXPORT engineTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    # Public engine headers (engine/include/engine/...).
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.hpp"
            PATTERN "*.inl"
    )

    # Vendored header-only dependencies the public headers expose (glm, imgui,
    # tracy, nlohmann, FastNoiseLite, Lua). Staged under include/lib so the same
    # include roots used in-tree (lib, lib/glm, lib/imgui, lib/tracy/public)
    # resolve identically against the install prefix.
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib/"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lib
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.hpp"
            PATTERN "*.inl"
    )

    # Runtime assets the engine itself loads (the default font). Shipped with
    # every install/package so a standalone consumer can stage them next to its
    # executable; engineAssets.cmake exposes ENGINE_ASSET_DIR (resolved relative
    # to itself, so it works from any prefix / Conan package folder).
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/assets/"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/sfs-engine/assets
        PATTERN ".DS_Store" EXCLUDE
    )

    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/engineAssets.cmake"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/sfs-engine/cmake
    )

    # sfs_generate_reflection / sfs_add_game_module, for projects built against
    # the package (included by engineConfig.cmake).
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sfsCodegen.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/engine
    )

    # The generated CMake package config (engineConfig.cmake) backs a plain
    # `find_package(engine)` against a raw `cmake --install` tree. A Conan
    # package generates its own config from the recipe's package_info, so the
    # recipe turns this off to avoid shipping two competing configs.
    if(ENGINE_INSTALL_CMAKE_PACKAGE)
        install(EXPORT engineTargets
            FILE engineTargets.cmake
            NAMESPACE sfs::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/engine
        )

        set(ENGINE_INSTALL_ASSETDIR "${CMAKE_INSTALL_DATADIR}/sfs-engine/assets")
        configure_package_config_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/engineConfig.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/engineConfig.cmake"
            INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/engine
            PATH_VARS ENGINE_INSTALL_ASSETDIR
        )

        write_basic_package_version_file(
            "${CMAKE_CURRENT_BINARY_DIR}/engineConfigVersion.cmake"
            VERSION 0.1.0
            COMPATIBILITY SameMajorVersion
        )

        install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/engineConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/engineConfigVersion.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/engine
        )
    endif()
endif()

# Engine tests (native only; the web build strips the desktop toolchain). Added
# after both targets exist: core/ tests link engine-core, runtime/ tests link
# engine. enable_testing() is called at the root so ctest finds them.
if(NOT ENGINE_WEB_BUILD AND ENGINE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
