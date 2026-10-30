# SfsEditor: the engine's editor application. It consumes the engine strictly
# through its package interface (the sfs:: targets), exactly as an external
# project would: configured on its own it find_package()s an installed engine;
# inside the engine workspace the same sfs:: targets already exist.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.24)
    project(SfsEditor LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

if(NOT TARGET sfs::engine-webgpu)
    find_package(engine REQUIRED)
endif()

if(NOT TARGET sfs::engine-webgpu)
    message(STATUS "SfsEditor needs engine-webgpu (native shared build only); skipping")
    return()
endif()

file(GLOB_RECURSE EDITOR_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

# ImGuizmo: the viewport's move/rotate/scale gizmos (vendored; see VENDORED.md).
set(EDITOR_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/lib/imguizmo/ImGuizmo.cpp")

add_executable(SfsEditor ${EDITOR_SRC} ${EDITOR_THIRD_PARTY})

target_include_directories(SfsEditor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib"
)

target_link_libraries(SfsEditor PRIVATE
    sfs::engine-webgpu
    "$<$<PLATFORM_ID:Windows>:ole32;shell32;uuid>" # the folder picker
)

target_compile_options(SfsEditor PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
)

set_target_properties(SfsEditor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

sfs_deploy_runtime(SfsEditor)

# Export ships the player built alongside the editor.
if(TARGET SfsPlayer)
    add_dependencies(SfsEditor SfsPlayer)
endif()

# The Code panel's default Neovim config ships next to the executable, copied
# whenever it changes (not only when the editor relinks).
file(GLOB_RECURSE SFS_EDITOR_NVIM_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/nvim/*")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/nvim.stamp"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/nvim" "$<TARGET_FILE_DIR:SfsEditor>/nvim"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/nvim.stamp"
    DEPENDS ${SFS_EDITOR_NVIM_FILES}
    VERBATIM)
add_custom_target(SfsEditorNvimConfig DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/nvim.stamp")
add_dependencies(SfsEditor SfsEditorNvimConfig)

# --- Building game modules ----------------------------------------------------
# The editor compiles a project's scripts into a module with the same toolchain
# and engine it was built with: bake those into editorBuildConfig.h.

# An installed copy of this engine (headers, import libraries, CMake package)
# for projects to build against. Refreshed whenever the engine changes.
set(SFS_EDITOR_ENGINE_PREFIX "${CMAKE_BINARY_DIR}/sdk")
if(TARGET engine-core) # in-tree: install the engine we just built
    add_custom_target(engine-sdk
        COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --prefix "${SFS_EDITOR_ENGINE_PREFIX}"
        COMMENT "Installing the engine SDK for game modules"
        VERBATIM)
    add_dependencies(engine-sdk engine-core engine engine-webgpu)
    add_dependencies(SfsEditor engine-sdk)
else() # against an installed engine: build modules against that same install
    get_filename_component(SFS_EDITOR_ENGINE_PREFIX "${engine_DIR}/../../.." ABSOLUTE)
endif()

# A Windows editor cross-built from Linux (WSL) runs its builds back in WSL.
if(CMAKE_CROSSCOMPILING AND WIN32 AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(SFS_EDITOR_BUILD_RUNNER "wsl")
    set(SFS_EDITOR_WSL_DISTRO "$ENV{WSL_DISTRO_NAME}")
    if(NOT SFS_EDITOR_WSL_DISTRO)
        set(SFS_EDITOR_WSL_DISTRO "Ubuntu")
    endif()
else()
    set(SFS_EDITOR_BUILD_RUNNER "native")
    set(SFS_EDITOR_WSL_DISTRO "")
endif()

if(SFS_CODEGEN_EXECUTABLE)
    set(SFS_EDITOR_CODEGEN "${SFS_CODEGEN_EXECUTABLE}")
else()
    set(SFS_EDITOR_CODEGEN "${CMAKE_BINARY_DIR}/bin/sfs-codegen")
endif()
set(SFS_EDITOR_TOOLCHAIN_FILE "${CMAKE_TOOLCHAIN_FILE}")
# The engine's source, which a web export builds for the browser (in-tree only:
# an installed engine has no sources, and can't export for the web).
if(TARGET engine-core)
    get_filename_component(SFS_EDITOR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
else()
    set(SFS_EDITOR_SOURCE_DIR "")
endif()
set(SFS_EDITOR_BUILD_TYPE "${CMAKE_BUILD_TYPE}")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/build/editorBuildConfig.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/editorBuildConfig.h")
target_include_directories(SfsEditor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(ENGINE_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(tests)
endif()
