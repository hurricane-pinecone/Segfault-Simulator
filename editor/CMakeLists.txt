# SfsEditor: the engine's editor application. It consumes the engine strictly
# through its package interface (the sfs:: targets), exactly as an external
# project would: configured on its own it find_package()s an installed engine;
# inside the engine workspace the same sfs:: targets already exist.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.24)
    project(SfsEditor LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

if(NOT TARGET sfs::engine-webgpu)
    find_package(engine REQUIRED)
endif()

if(NOT TARGET sfs::engine-webgpu)
    message(STATUS "SfsEditor needs engine-webgpu (native shared build only); skipping")
    return()
endif()

file(GLOB_RECURSE EDITOR_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(SfsEditor ${EDITOR_SRC})

target_include_directories(SfsEditor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(SfsEditor PRIVATE
    sfs::engine-webgpu
)

target_compile_options(SfsEditor PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
)

set_target_properties(SfsEditor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

sfs_deploy_runtime(SfsEditor)
