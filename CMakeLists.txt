cmake_minimum_required(VERSION 3.24)

project(MyEngineWorkspace LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build only engine-core (+ tests): skips the render library, the sample game,
# and everything that needs SDL/OpenGL/Conan. Lets the test pipeline configure
# and build with no third-party dependencies.
option(ENGINE_CORE_ONLY "Build only engine-core and its tests" OFF)

# Build the bundled sample game. Off when packaging the engine on its own (the
# Conan recipe sets this) so the engine libs can be built/installed standalone.
option(ENGINE_BUILD_SAMPLE "Build the bundled sample game" ON)

# Enable testing at the top level so `ctest --test-dir <build>` finds tests
# registered in subdirectories. The engine owns its own tests (engine/tests).
enable_testing()

add_subdirectory(engine)

if(NOT ENGINE_CORE_ONLY AND ENGINE_BUILD_SAMPLE)
    add_subdirectory(sampleGame)
    add_subdirectory(samplePlatformer)
endif()
