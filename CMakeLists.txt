cmake_minimum_required(VERSION 3.24)

project(MyEngineWorkspace LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build only engine-core (+ tests): skips the render library, the samples,
# and everything that needs SDL/WebGPU/Conan. Lets the test pipeline configure
# and build with no third-party dependencies.
option(ENGINE_CORE_ONLY "Build only engine-core and its tests" OFF)

# Build the bundled sample games for the web (samples/, as web pages). Off when
# packaging the engine on its own (the Conan recipe sets this), and off by
# default when a game pulls the engine in with add_subdirectory.
option(ENGINE_BUILD_SAMPLE "Build the bundled sample games" ${PROJECT_IS_TOP_LEVEL})

# Build the engine's test suite. Off by default when a game pulls the engine in
# with add_subdirectory, so the game's build doesn't compile engine tests.
option(ENGINE_BUILD_TESTS "Build the engine test suite" ${PROJECT_IS_TOP_LEVEL})

# Build the editor application (SfsEditor). Off by default when a game pulls the
# engine in with add_subdirectory.
option(ENGINE_BUILD_EDITOR "Build the SfsEditor application" ${PROJECT_IS_TOP_LEVEL})

# Enable testing at the top level so `ctest --test-dir <build>` finds tests
# registered in subdirectories. The engine owns its own tests (engine/tests).
enable_testing()

add_subdirectory(engine)

# Desktop only: the editor and the player (what it exports games as) need the
# native WebGPU engine, which a web build doesn't have. (The engine sets
# ENGINE_WEB_BUILD in its own scope, so test the toolchain's flag here.)
if(NOT ENGINE_CORE_ONLY AND ENGINE_BUILD_EDITOR AND NOT EMSCRIPTEN)
    add_subdirectory(player) # what the editor exports games as
    add_subdirectory(editor)
endif()

if(NOT ENGINE_CORE_ONLY AND ENGINE_BUILD_SAMPLE)
    if(EMSCRIPTEN)
        # The site: the sample projects (made in the editor) as web pages, the
        # 3D showcase at the root and the platformer under samplePlatformer/.
        # They need a host sfs-codegen (SFS_CODEGEN_EXECUTABLE) for their scene
        # files.
        if(SFS_CODEGEN_EXECUTABLE)
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/showcase3d/project.sfsproj")
                sfs_add_web_game(showcase3d
                    PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/samples/showcase3d"
                    OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin"
                    HINT "W/S run · A/D turn · Space jump (twice) · Shift dash · drag to look · T hurries the day · click the canvas first")
            endif()
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/platformer2d/project.sfsproj")
                sfs_add_web_game(platformer2d
                    PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/samples/platformer2d"
                    OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/samplePlatformer"
                    HINT "A/D or arrows run · Space or W jump (again in the air) · Shift dash · R restart · click the game first")
            endif()
        else()
            message(WARNING "No SFS_CODEGEN_EXECUTABLE: the sample games aren't built for the web")
        endif()
    endif()
    # On the desktop the sample projects (samples/) are opened and built in
    # the editor.
endif()
