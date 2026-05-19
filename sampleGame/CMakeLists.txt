set(IS_WEB_BUILD FALSE)

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR EMSCRIPTEN)
    set(IS_WEB_BUILD TRUE)
endif()

file(GLOB_RECURSE GAME_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(sampleGame
    ${GAME_SRC}
)

target_include_directories(sampleGame PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(sampleGame PRIVATE
    engine
)

set_target_properties(sampleGame PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(IS_WEB_BUILD)
    set_target_properties(sampleGame PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        OUTPUT_NAME "index"
        SUFFIX ".html"
    )

    target_compile_options(engine PUBLIC
        -pthread
        -matomics
        -mbulk-memory
    )

    target_link_options(sampleGame PRIVATE
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
        # -sPROXY_TO_PTHREAD=1
        # -sOFFSCREENCANVAS_SUPPORT=1
    )

  add_custom_target(run
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/web/server.py"
          "${CMAKE_BINARY_DIR}/bin/server.py"
      COMMAND python3 "${CMAKE_BINARY_DIR}/bin/server.py"
      DEPENDS sampleGame
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  )

add_custom_command(TARGET sampleGame POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/web/server.py"
        "$<TARGET_FILE_DIR:sampleGame>/server.py"
)
else()
    add_custom_command(TARGET sampleGame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:sampleGame>/assets"
    )

    add_custom_target(run
        COMMAND $<TARGET_FILE:sampleGame>
        DEPENDS sampleGame
        WORKING_DIRECTORY $<TARGET_FILE_DIR:sampleGame>
    )
endif()
