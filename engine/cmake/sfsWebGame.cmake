# sfs_add_web_game: build an editor project (project.sfsproj, scenes/, assets/,
# prefabs/, scripts/) into a web page that plays it: the player, with the
# project's scripts compiled in (a web page can't load a game module) and its
# content preloaded. Needs the engine built for the web, with a host
# sfs-codegen (SFS_CODEGEN_EXECUTABLE).
#
#   sfs_add_web_game(<target> PROJECT <dir>
#       [OUTPUT_DIR <dir>]        # where index.html etc. go (default: bin/<target>)
#       [WIDTH <px>] [HEIGHT <px>] # the canvas's size (default 1280 x 720)
#       [HINT <text>])            # a line under the game (its controls, say)
#
# The output folder is a static site: index.html, index.js, index.wasm,
# index.data, and the service worker that lets it use threads on hosts that
# don't send cross-origin isolation headers (GitHub Pages).

function(sfs_add_web_game target)
    # The engine's folder (this file's parent), wherever the call is from.
    set(SFS_WEB_GAME_ENGINE_DIR "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    cmake_parse_arguments(ARG "" "PROJECT;OUTPUT_DIR;WIDTH;HEIGHT;HINT" "" ${ARGN})
    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/${target}")
    endif()
    if(NOT ARG_WIDTH)
        set(ARG_WIDTH 1280)
    endif()
    if(NOT ARG_HEIGHT)
        set(ARG_HEIGHT 720)
    endif()
    if(NOT COMMAND sfs_generate_reflection OR NOT SFS_CODEGEN_EXECUTABLE)
        message(FATAL_ERROR "sfs_add_web_game needs a host sfs-codegen: pass "
                            "-DSFS_CODEGEN_EXECUTABLE=<native-build>/bin/sfs-codegen")
    endif()

    # The project: its name, the scene it starts in, and its module's name
    # (as the editor names it: Project::moduleName()).
    set(project_file "${ARG_PROJECT}/project.sfsproj")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${project_file}")
    file(READ "${project_file}" project)
    string(JSON name GET "${project}" name)
    string(JSON start_scene ERROR_VARIABLE no_scene GET "${project}" startScene)
    if(no_scene)
        message(FATAL_ERROR "${project_file} names no start scene")
    endif()
    string(REGEX REPLACE "[^A-Za-z0-9]" "_" module "${name}")
    if(module STREQUAL "" OR module MATCHES "^[0-9]")
        set(module "Game${module}")
    endif()

    # The host codegen (the engine's own variables are in its directory's scope).
    set(SFS_CODEGEN_COMMAND "${SFS_CODEGEN_EXECUTABLE}")
    set(SFS_CODEGEN_DEPENDS "${SFS_CODEGEN_EXECUTABLE}")

    # The player, with the scripts and their registration compiled in.
    set(scripts "${ARG_PROJECT}/scripts")
    file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${scripts}/*.h")
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${scripts}/*.cpp")
    list(FILTER sources EXCLUDE REGEX "\\.ui\\.cpp$") # the editor's previews
    set(player "${SFS_WEB_GAME_ENGINE_DIR}/../player/src")
    add_executable(${target} "${player}/main.cpp" "${player}/playerApp.cpp" ${sources})
    target_include_directories(${target} PRIVATE "${scripts}" "${player}")
    target_link_libraries(${target} PRIVATE sfs::engine-webgpu)
    target_compile_definitions(${target} PRIVATE SFS_STATIC_GAME_MODULE=1)
    sfs_generate_reflection(${target}
        MODULE ${module}
        FUNCTION sfsRegisterModule
        FUNCTION_HEADER engine/runtime/scripting/gameModule.h
        INCLUDE_ROOT "${scripts}"
        HEADERS ${headers})

    # game.json, as an export writes it (no module: it's compiled in).
    set(staging "${CMAKE_CURRENT_BINARY_DIR}/${target}-web")
    file(MAKE_DIRECTORY "${staging}")
    file(WRITE "${staging}/game.json"
        "{\n  \"formatVersion\": 1,\n  \"name\": \"${name}\",\n"
        "  \"moduleName\": \"${module}\",\n  \"startScene\": \"${start_scene}\",\n"
        "  \"windowWidth\": ${ARG_WIDTH},\n  \"windowHeight\": ${ARG_HEIGHT}\n}\n")

    # The page: the engine's shell, filled in for this game.
    file(READ "${SFS_WEB_GAME_ENGINE_DIR}/web/shell.html" shell)
    math(EXPR aspect_milli "${ARG_WIDTH} * 1000 / ${ARG_HEIGHT}")
    math(EXPR aspect_whole "${aspect_milli} / 1000")
    math(EXPR aspect_frac "${aspect_milli} % 1000")
    string(LENGTH "${aspect_frac}" frac_digits)
    if(frac_digits EQUAL 1)
        set(aspect_frac "00${aspect_frac}")
    elseif(frac_digits EQUAL 2)
        set(aspect_frac "0${aspect_frac}")
    endif()
    string(REPLACE "{{{ SFS_GAME_TITLE }}}" "${name}" shell "${shell}")
    string(REPLACE "{{{ SFS_GAME_HINT }}}" "${ARG_HINT}" shell "${shell}")
    string(REPLACE "{{{ SFS_GAME_ASPECT }}}" "${aspect_whole}.${aspect_frac}" shell "${shell}")
    file(WRITE "${staging}/shell.html" "${shell}")

    # What it reads, at the same paths as an exported game's folder: the
    # engine's assets and the project's merge into /assets.
    # A change to any of it (a scene saved in the editor) packs it again.
    set(preload
        "--preload-file=${staging}/game.json@/game.json"
        "--preload-file=${SFS_WEB_GAME_ENGINE_DIR}/include/assets@/assets")
    file(GLOB_RECURSE content CONFIGURE_DEPENDS "${SFS_WEB_GAME_ENGINE_DIR}/include/assets/*")
    foreach(folder scenes assets prefabs)
        if(IS_DIRECTORY "${ARG_PROJECT}/${folder}")
            list(APPEND preload "--preload-file=${ARG_PROJECT}/${folder}@/${folder}")
            file(GLOB_RECURSE files CONFIGURE_DEPENDS "${ARG_PROJECT}/${folder}/*")
            list(APPEND content ${files})
        endif()
    endforeach()

    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${ARG_OUTPUT_DIR}"
        OUTPUT_NAME "index"
        SUFFIX ".html"
        LINK_DEPENDS "${staging}/shell.html;${staging}/game.json;${content}")
    target_link_options(${target} PRIVATE
        --shell-file "${staging}/shell.html"
        ${preload}
        -sALLOW_MEMORY_GROWTH=1
        -sASYNCIFY
        -sEXIT_RUNTIME=0)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SFS_WEB_GAME_ENGINE_DIR}/web/coi-serviceworker.js"
            "${SFS_WEB_GAME_ENGINE_DIR}/web/favicon.png"
            "$<TARGET_FILE_DIR:${target}>")
endfunction()
