# A flat 2D platformer built on the engine, proving the generic (non-isometric)
# render path. Links the engine's namespaced target like any consumer; uses the
# default flat OpenGLQuadRenderer (no isometric backend).

file(GLOB_RECURSE PLATFORMER_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(samplePlatformer ${PLATFORMER_SRC})

target_include_directories(samplePlatformer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(samplePlatformer PRIVATE sfs::engine)

set_target_properties(samplePlatformer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_custom_command(TARGET samplePlatformer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:samplePlatformer>/assets"
)

add_custom_target(run-platformer
    COMMAND $<TARGET_FILE:samplePlatformer>
    DEPENDS samplePlatformer
    WORKING_DIRECTORY $<TARGET_FILE_DIR:samplePlatformer>
)
