# Standalone-capable. When configured on its own (not pulled in by the engine
# workspace's add_subdirectory), bootstrap the project and consume the engine as
# an installed/Conan package -- exactly what an external user does. The in-tree
# workspace build and the web build instead reach this file with sfs::engine
# already defined, and skip straight to building the executable.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.24)
    project(sampleGame LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

set(IS_WEB_BUILD FALSE)

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR EMSCRIPTEN)
    set(IS_WEB_BUILD TRUE)
endif()

# Resolve the engine. In-tree (workspace + web) the target already exists from
# add_subdirectory; standalone, find the package an external consumer installed.
if(NOT TARGET sfs::engine)
    find_package(engine REQUIRED)
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
    sfs::engine
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

    # The shell is a link input (--shell-file); without this, editing it doesn't
    # relink and the embedded HTML goes stale.
    set_target_properties(sampleGame PROPERTIES
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
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

        # Expose the Lua entry points so the shell's editor can hot-edit the
        # running game via Module.ccall (keep _main alongside them).
        -sEXPORTED_FUNCTIONS=['_main','_sfsEvalLua','_sfsLuaKeys']
        -sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']
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

      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/web/coi-serviceworker.js"
          "$<TARGET_FILE_DIR:sampleGame>/coi-serviceworker.js"

      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/web/favicon.png"
          "$<TARGET_FILE_DIR:sampleGame>/favicon.png"
  )
else()
    add_custom_command(TARGET sampleGame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:sampleGame>/assets"
    )

    # Standalone (package) build: the engine isn't built in-tree, so stage its
    # runtime assets (default font) from the package next to the executable.
    # ENGINE_ASSET_DIR is set by the engine's CMake/Conan config. In the
    # workspace build the engine already copies these to bin, so this is skipped.
    if(DEFINED ENGINE_ASSET_DIR)
        add_custom_command(TARGET sampleGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${ENGINE_ASSET_DIR}"
                "$<TARGET_FILE_DIR:sampleGame>/assets"
        )
    endif()

    add_custom_target(run
        COMMAND $<TARGET_FILE:sampleGame>
        DEPENDS sampleGame
        WORKING_DIRECTORY $<TARGET_FILE_DIR:sampleGame>
    )
endif()
