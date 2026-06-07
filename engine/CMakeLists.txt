set(ENGINE_WEB_BUILD FALSE)

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR EMSCRIPTEN)
    set(ENGINE_WEB_BUILD TRUE)
endif()

message(STATUS "ENGINE_WEB_BUILD=${ENGINE_WEB_BUILD}")
message(STATUS "ENGINE_CORE_ONLY=${ENGINE_CORE_ONLY}")

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

add_library(engine-core STATIC ${ENGINE_CORE_SRC} ${LUA_SRC})

if(ENGINE_SANITIZE)
    # FastNoiseLite's hash multiply relies on signed integer overflow wrapping,
    # defined by convention but flagged by UBSan. noise.cpp is the only unit that
    # includes it, so drop just that one check for just this file.
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/noise/noise.cpp"
        PROPERTIES COMPILE_OPTIONS -fno-sanitize=signed-integer-overflow)
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

file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

add_library(engine STATIC
    ${ENGINE_RENDER_SRC}
    ${ENGINE_HEADERS}
)

add_library(sfs::engine ALIAS engine)

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

    target_include_directories(engine PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/lib/imgui>"
    )

    target_sources(engine PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_sdl2.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_opengl3.cpp"
    )

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
if(NOT ENGINE_WEB_BUILD)
    add_subdirectory(tests)
endif()
