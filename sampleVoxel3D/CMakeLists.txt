# Tiny-voxel 3D proof-of-concept: a true-3D MVP render path (orthographic orbit
# camera + Teardown-style flat-coloured cubes) standing alongside the iso engine.
# Native only for the POC -- no web shell is wired up, so the web build skips it.

set(IS_WEB_BUILD FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR EMSCRIPTEN)
    set(IS_WEB_BUILD TRUE)
endif()

if(NOT IS_WEB_BUILD)
    file(GLOB_RECURSE VOXEL3D_SRC CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    )

    add_executable(sampleVoxel3D ${VOXEL3D_SRC})

    target_include_directories(sampleVoxel3D PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )

    target_link_libraries(sampleVoxel3D PRIVATE sfs::engine)

    set_target_properties(sampleVoxel3D PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_custom_target(run-voxel3d
        COMMAND $<TARGET_FILE:sampleVoxel3D>
        DEPENDS sampleVoxel3D
        WORKING_DIRECTORY $<TARGET_FILE_DIR:sampleVoxel3D>
    )
endif()
