cmake_minimum_required(VERSION 3.24)

project(MyEngineWorkspace LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build only engine-core (+ tests): skips the render library, the sample game,
# and everything that needs SDL/WebGPU/Conan. Lets the test pipeline configure
# and build with no third-party dependencies.
option(ENGINE_CORE_ONLY "Build only engine-core and its tests" OFF)

# Build the bundled sample games. Off when packaging the engine on its own (the
# Conan recipe sets this) so the engine libs can be built/installed standalone,
# and off by default when a game pulls the engine in with add_subdirectory.
option(ENGINE_BUILD_SAMPLE "Build the bundled sample games" ${PROJECT_IS_TOP_LEVEL})

# Build the engine's test suite. Off by default when a game pulls the engine in
# with add_subdirectory, so the game's build doesn't compile engine tests.
option(ENGINE_BUILD_TESTS "Build the engine test suite" ${PROJECT_IS_TOP_LEVEL})

# Build the editor application (SfsEditor). Off by default when a game pulls the
# engine in with add_subdirectory.
option(ENGINE_BUILD_EDITOR "Build the SfsEditor application" ${PROJECT_IS_TOP_LEVEL})

# Enable testing at the top level so `ctest --test-dir <build>` finds tests
# registered in subdirectories. The engine owns its own tests (engine/tests).
enable_testing()

add_subdirectory(engine)

# Desktop only: the editor and the player (what it exports games as) need the
# native WebGPU engine, which a web build doesn't have. (The engine sets
# ENGINE_WEB_BUILD in its own scope, so test the toolchain's flag here.)
if(NOT ENGINE_CORE_ONLY AND ENGINE_BUILD_EDITOR AND NOT EMSCRIPTEN)
    add_subdirectory(player) # what the editor exports games as
    add_subdirectory(editor)
endif()

if(NOT ENGINE_CORE_ONLY AND ENGINE_BUILD_SAMPLE)
    add_subdirectory(sampleGame)
    add_subdirectory(samplePlatformer)
endif()
