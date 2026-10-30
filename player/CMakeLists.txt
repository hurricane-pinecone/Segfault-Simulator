# SfsPlayer: the executable an exported game runs as. The editor's Export copies
# it (renamed after the game) next to the game's module, scenes and assets; it
# reads game.json to know what to load. Like the editor, it consumes the engine
# only through its package interface.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.24)
    project(SfsPlayer LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

if(NOT TARGET sfs::engine-webgpu)
    find_package(engine REQUIRED)
endif()

if(NOT TARGET sfs::engine-webgpu)
    message(STATUS "SfsPlayer needs engine-webgpu (native shared build only); skipping")
    return()
endif()

add_executable(SfsPlayer
    src/main.cpp
    src/playerApp.cpp
)

target_link_libraries(SfsPlayer PRIVATE sfs::engine-webgpu)

# On Windows a game opens no console window beside its own (its log is in its
# folder under %APPDATA%, and goes to the terminal it's started from, if
# any; see PlayerApp::load). The executable carries version information, and
# an icon when the build is given one.
if(WIN32)
    set_target_properties(SfsPlayer PROPERTIES WIN32_EXECUTABLE ON)
    if(MSVC)
        # A GUI program's entry point is WinMain; the player's is main.
        target_link_options(SfsPlayer PRIVATE /ENTRY:mainCRTStartup)
    endif()
    set(SFS_PLAYER_ICON "" CACHE FILEPATH "An .ico for the player (and so for exported games); empty for none")
    set(SFS_PLAYER_VERSION "0.1.0")
    string(REPLACE "." "," SFS_PLAYER_VERSION_COMMAS "${SFS_PLAYER_VERSION},0")
    if(SFS_PLAYER_ICON)
        file(TO_CMAKE_PATH "${SFS_PLAYER_ICON}" icon)
        set(SFS_PLAYER_ICON_RESOURCE "1 ICON \"${icon}\"")
    else()
        set(SFS_PLAYER_ICON_RESOURCE "")
    endif()
    configure_file(src/player.rc.in "${CMAKE_CURRENT_BINARY_DIR}/player.rc" @ONLY)
    target_sources(SfsPlayer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/player.rc")
endif()

target_compile_options(SfsPlayer PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
)

set_target_properties(SfsPlayer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    # An exported game keeps its libraries next to the executable.
    BUILD_RPATH_USE_ORIGIN ON
    INSTALL_RPATH "$ORIGIN"
)
if(UNIX AND NOT APPLE)
    target_link_options(SfsPlayer PRIVATE "-Wl,-rpath,$ORIGIN")
endif()

sfs_deploy_runtime(SfsPlayer)

# Where the engine libraries aren't next to the player (Linux builds keep them
# in their own folders), list them for the editor's Export to ship.
if(NOT WIN32)
    set(player_libs "")
    foreach(lib sfs::engine-core sfs::engine sfs::engine-webgpu)
        get_target_property(lib_type ${lib} TYPE)
        if(lib_type STREQUAL "SHARED_LIBRARY")
            string(APPEND player_libs "$<TARGET_FILE:${lib}>\n")
        endif()
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/bin/SfsPlayer.libs" CONTENT "${player_libs}")
endif()
