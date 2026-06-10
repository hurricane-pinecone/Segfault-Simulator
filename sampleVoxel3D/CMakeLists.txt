# Tiny-voxel 3D game: a WebGPU compute voxel renderer (brickmap + raymarch + GPU
# water cellular-automata) wired through the engine as Voxel3DGame/Voxel3DScene
# on the WebGPU render backend. Links engine-webgpu, which carries the backend +
# GPU-voxel subsystem and the wgpu-native runtime. Native only -- engine-webgpu
# is not built on the web toolchain, so this target is skipped there.
if(NOT TARGET sfs::engine-webgpu)
    message(STATUS "sampleVoxel3D needs engine-webgpu (native build only); skipping")
    return()
endif()

file(GLOB_RECURSE VOXEL3D_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(sampleVoxel3D ${VOXEL3D_SRC})

target_include_directories(sampleVoxel3D PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(sampleVoxel3D PRIVATE
    sfs::engine-webgpu
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
