# sfs_generate_reflection: run sfs-codegen over script headers and compile the
# generated registration into a target.
#
#   sfs_generate_reflection(<target>
#       MODULE <name>              # stored on each registered type
#       FUNCTION <qualified-name>  # registration function to define
#       HEADERS <header>...        # one script type per header
#       [FUNCTION_HEADER <header>] # declares FUNCTION (default: reflection.h)
#       [INCLUDE_ROOT <dir>])      # include headers relative to this
#
# The tool is SFS_CODEGEN_COMMAND: the in-tree sfs-codegen target in a native
# build, or the host executable in SFS_CODEGEN_EXECUTABLE when cross-compiling.
# Against an installed engine, engineConfig.cmake sets it up.
function(sfs_generate_reflection target)
    cmake_parse_arguments(ARG "" "MODULE;FUNCTION;FUNCTION_HEADER;INCLUDE_ROOT" "HEADERS" ${ARGN})

    set(output "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}Reflection.gen.cpp")
    set(args --module "${ARG_MODULE}" --function "${ARG_FUNCTION}" --output "${output}")
    if(ARG_FUNCTION_HEADER)
        list(APPEND args --function-header "${ARG_FUNCTION_HEADER}")
    endif()
    if(ARG_INCLUDE_ROOT)
        list(APPEND args --include-root "${ARG_INCLUDE_ROOT}")
    endif()

    add_custom_command(
        OUTPUT "${output}"
        COMMAND ${SFS_CODEGEN_COMMAND} ${args} ${ARG_HEADERS}
        DEPENDS ${SFS_CODEGEN_DEPENDS} ${ARG_HEADERS}
        COMMENT "Generating reflection for ${target}"
        VERBATIM)
    target_sources(${target} PRIVATE "${output}")
endfunction()

# sfs_add_game_module: build a project's scripts into a game module -- the
# shared library the editor loads (and reloads) to run them.
#
#   sfs_add_game_module(<name> [SCRIPTS_DIR <dir>])
#
# Every header in SCRIPTS_DIR (default: ./scripts) is scanned by sfs-codegen;
# every .cpp there is compiled in. The module lands in <build>/module/, and
# exports only its entry point (see engine/runtime/scripting/gameModule.h).
function(sfs_add_game_module name)
    cmake_parse_arguments(ARG "" "SCRIPTS_DIR" "" ${ARGN})
    if(NOT ARG_SCRIPTS_DIR)
        set(ARG_SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/scripts")
    endif()

    file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${ARG_SCRIPTS_DIR}/*.h")
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${ARG_SCRIPTS_DIR}/*.cpp")

    add_library(${name} SHARED ${sources})
    target_link_libraries(${name} PRIVATE sfs::engine)
    target_include_directories(${name} PRIVATE "${ARG_SCRIPTS_DIR}")
    set_target_properties(${name} PROPERTIES
        PREFIX ""
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/module"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/module")

    sfs_generate_reflection(${name}
        MODULE ${name}
        FUNCTION sfsRegisterModule
        FUNCTION_HEADER engine/runtime/scripting/gameModule.h
        INCLUDE_ROOT "${ARG_SCRIPTS_DIR}"
        HEADERS ${headers})

    # The editor's UI Preview module: the UI files (*.ui.cpp) and a preview
    # of each view in the UI headers (*.ui.h), filled with placeholders. Only
    # built when asked for (the editor does, as UI files are saved): it's
    # unoptimised, with the engine's headers precompiled, to build in seconds,
    # and loads apart from the game module, which stays as it is.
    file(GLOB_RECURSE ui_headers CONFIGURE_DEPENDS "${ARG_SCRIPTS_DIR}/*.ui.h")
    file(GLOB_RECURSE ui_sources CONFIGURE_DEPENDS "${ARG_SCRIPTS_DIR}/*.ui.cpp")
    set(preview ${name}UiPreview)
    set(preview_source "${CMAKE_CURRENT_BINARY_DIR}/generated/${preview}.gen.cpp")
    add_custom_command(
        OUTPUT "${preview_source}"
        COMMAND ${SFS_CODEGEN_COMMAND} --ui-previews
                --module ${preview} --function sfsRegisterModule
                --include-root "${ARG_SCRIPTS_DIR}"
                --output "${preview_source}" ${ui_headers}
        DEPENDS ${SFS_CODEGEN_DEPENDS} ${ui_headers}
        COMMENT "Generating UI previews for ${name}"
        VERBATIM)
    add_library(${preview} SHARED EXCLUDE_FROM_ALL ${ui_sources} "${preview_source}")
    target_link_libraries(${preview} PRIVATE sfs::engine)
    target_include_directories(${preview} PRIVATE "${ARG_SCRIPTS_DIR}")
    # After the build type's flags, so it wins.
    target_compile_options(${preview} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/Od,-O0>)
    target_precompile_headers(${preview} PRIVATE <sfs/sfs.h> <functional> <string> <vector>)
    set_target_properties(${preview} PROPERTIES
        PREFIX ""
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ui-preview"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ui-preview")

    # clangd (and most editors) look for the compile database in the project:
    # link it at <project>/.sfs/compile_commands.json, which the project's
    # .clangd points to. CMake writes the database itself at generate time.
    if(CMAKE_EXPORT_COMPILE_COMMANDS)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.sfs")
        file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
             "${CMAKE_CURRENT_SOURCE_DIR}/.sfs/compile_commands.json"
             RESULT link_result SYMBOLIC)
        if(NOT link_result EQUAL 0)
            message(STATUS "Couldn't link compile_commands.json into ${CMAKE_CURRENT_SOURCE_DIR}/.sfs: ${link_result}")
        endif()
    endif()
endfunction()
