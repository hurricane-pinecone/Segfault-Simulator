# A flat 2D platformer built on the engine, proving the generic (non-isometric)
# render path. Links the engine's namespaced target like any consumer; uses the
# default flat OpenGLQuadRenderer (no isometric backend).

set(IS_WEB_BUILD FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR EMSCRIPTEN)
    set(IS_WEB_BUILD TRUE)
endif()

file(GLOB_RECURSE PLATFORMER_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(samplePlatformer ${PLATFORMER_SRC})

target_include_directories(samplePlatformer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(samplePlatformer PRIVATE sfs::engine)

if(IS_WEB_BUILD)
    # Web build deploys under <pages-url>/samplePlatformer, so output into a
    # samplePlatformer/ subdir of the Pages artifact (build-web/bin) as
    # index.html. No Lua editor here, so the shell is minimal.
    set_target_properties(samplePlatformer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/samplePlatformer"
        OUTPUT_NAME "index"
        SUFFIX ".html"
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
    )

    target_link_options(samplePlatformer PRIVATE
        --shell-file "${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"

        "--preload-file=${CMAKE_CURRENT_SOURCE_DIR}/assets@/assets"
        "--preload-file=${CMAKE_SOURCE_DIR}/engine/include/assets@/assets"

        -sALLOW_MEMORY_GROWTH=1
        -sASYNCIFY
        -sUSE_SDL=2
        -sUSE_SDL_IMAGE=2
        -sSDL2_IMAGE_FORMATS=["png"]
        -sUSE_SDL_TTF=2
        -sMIN_WEBGL_VERSION=2
        -sMAX_WEBGL_VERSION=2
        -sFULL_ES3=1
        -sUSE_PTHREADS=1
        -sPTHREAD_POOL_SIZE=4
    )

    add_custom_command(TARGET samplePlatformer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/web/coi-serviceworker.js"
            "$<TARGET_FILE_DIR:samplePlatformer>/coi-serviceworker.js"

        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/web/favicon.png"
            "$<TARGET_FILE_DIR:samplePlatformer>/favicon.png"

        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/web/server.py"
            "$<TARGET_FILE_DIR:samplePlatformer>/server.py"
    )

    add_custom_target(run-platformer
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/web/server.py"
            "$<TARGET_FILE_DIR:samplePlatformer>/server.py"
        COMMAND python3 "$<TARGET_FILE_DIR:samplePlatformer>/server.py"
        DEPENDS samplePlatformer
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:samplePlatformer>"
    )
else()
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
endif()
