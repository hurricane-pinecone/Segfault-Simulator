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

add_subdirectory(engine)

if(NOT ENGINE_CORE_ONLY)
    add_subdirectory(sampleGame)
endif()

# Native-only test targets (the web build strips the desktop toolchain).
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    enable_testing()
    add_subdirectory(tests)
endif()
