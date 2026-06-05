cmake_minimum_required(VERSION 3.24)

project(MyEngineWorkspace LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(engine)
add_subdirectory(sampleGame)

# Native-only test targets (the web build strips the desktop toolchain).
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    enable_testing()
    add_subdirectory(tests)
endif()
