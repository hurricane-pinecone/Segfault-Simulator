# Tiny-voxel 3D game -- being rebuilt on a WebGPU compute stack (brickmap +
# raymarch + GPU cellular-automata), off the OpenGL engine. Native only for now
# (wgpu-native); the web (Emscripten) shell is wired later. The old OpenGL
# sources (voxel3dGame, scenes/, tinyHeightmapGenerator) stay in the tree for
# porting and are not compiled here yet.

set(IS_WEB_BUILD FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR EMSCRIPTEN)
    set(IS_WEB_BUILD TRUE)
endif()

if(NOT IS_WEB_BUILD)
    include(FetchContent)

    # WebGPU via wgpu-native (prebuilt binaries; pinned tag, not a branch tip).
    FetchContent_Declare(webgpu
        GIT_REPOSITORY https://github.com/eliemichel/WebGPU-distribution.git
        GIT_TAG wgpu-v24.0.0.2
    )
    FetchContent_MakeAvailable(webgpu)

    # SDL2 (from Conan) + the SDL2->WGPUSurface helper.
    find_package(SDL2 REQUIRED)
    FetchContent_Declare(sdl2webgpu
        GIT_REPOSITORY https://github.com/eliemichel/sdl2webgpu.git
        GIT_TAG eda8bedcb78a755f42c5efa00b732c62e89c23cb # main @ 2024, v24-compatible
    )
    FetchContent_MakeAvailable(sdl2webgpu)

    add_executable(sampleVoxel3D
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    )

    target_link_libraries(sampleVoxel3D PRIVATE
        webgpu
        sdl2webgpu
        SDL2::SDL2
    )

    set_target_properties(sampleVoxel3D PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    # Copy the wgpu-native runtime library next to the executable.
    target_copy_webgpu_binaries(sampleVoxel3D)

    add_custom_target(run-voxel3d
        COMMAND $<TARGET_FILE:sampleVoxel3D>
        DEPENDS sampleVoxel3D
        WORKING_DIRECTORY $<TARGET_FILE_DIR:sampleVoxel3D>
    )
endif()
