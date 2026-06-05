set(ENGINE_WEB_BUILD FALSE)

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR EMSCRIPTEN)
    set(ENGINE_WEB_BUILD TRUE)
endif()

message(STATUS "ENGINE_WEB_BUILD=${ENGINE_WEB_BUILD}")
message(STATUS "ENGINE_CORE_ONLY=${ENGINE_CORE_ONLY}")

# ----------------------------------------------------------------------------
# engine-core: ECS, scripting, and plain-data subsystems. No SDL/GL/imgui and no
# Conan dependencies -- it builds against vendored Lua + glm alone, so a consumer
# (e.g. the test suite) can link it without the desktop/render stack.
# ----------------------------------------------------------------------------

file(GLOB_RECURSE ENGINE_CORE_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
)

# Vendored Lua 5.4 (minus the standalone interpreter/compiler mains), compiled
# into core on every platform so scripting needs no external Lua.
file(GLOB LUA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/lib/lua/*.c")
list(FILTER LUA_SRC EXCLUDE REGEX ".*/lua\\.c$")
list(FILTER LUA_SRC EXCLUDE REGEX ".*/luac\\.c$")

add_library(engine-core STATIC ${ENGINE_CORE_SRC} ${LUA_SRC})

target_include_directories(engine-core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/glm"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/lua"
)

target_compile_options(engine-core PRIVATE
    -Wall
    -Wno-unknown-warning-option
)

if(ENGINE_WEB_BUILD)
    target_compile_definitions(engine-core PUBLIC ENGINE_WEB=1)
    # Threads/atomics flags must match across every wasm object in the link.
    target_compile_options(engine-core PUBLIC -pthread -matomics -mbulk-memory)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(engine-core PUBLIC Threads::Threads)
endif()

# ----------------------------------------------------------------------------
# engine: the render/platform layer (SDL, OpenGL, imgui). Depends on engine-core.
# Skipped entirely by a core-only configure (e.g. the CI test job): no render
# lib, no SDL, nothing that needs Conan.
# ----------------------------------------------------------------------------
if(NOT ENGINE_CORE_ONLY)

file(GLOB_RECURSE ENGINE_RENDER_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/*.cpp"
)

file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

add_library(engine STATIC
    ${ENGINE_RENDER_SRC}
    ${ENGINE_HEADERS}
)

target_link_libraries(engine PUBLIC engine-core)

target_include_directories(engine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_options(engine PRIVATE
    -Wall
    -Wno-unknown-warning-option
)

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/include/assets
     DESTINATION ${CMAKE_BINARY_DIR}/bin)

if(NOT ENGINE_WEB_BUILD AND CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_include_directories(engine PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/tracy/public"
    )

    target_sources(engine PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/tracy/public/TracyClient.cpp"
    )

    target_compile_definitions(engine PUBLIC
        TRACY_ENABLE
    )
endif()

if(ENGINE_WEB_BUILD)
    target_compile_options(engine PUBLIC
        -sUSE_SDL=2
        -sUSE_SDL_IMAGE=2
        -sSDL2_IMAGE_FORMATS=["png"]
        -sUSE_SDL_TTF=2
        -sUSE_SDL_MIXER=2
    )

    target_link_options(engine PUBLIC
        -sUSE_SDL=2
        -sUSE_SDL_IMAGE=2
        -sSDL2_IMAGE_FORMATS=["png"]
        -sUSE_SDL_TTF=2
        -sUSE_SDL_MIXER=2
        -sMIN_WEBGL_VERSION=2
        -sMAX_WEBGL_VERSION=2
        -sFULL_ES3=1
        -sUSE_PTHREADS=1
        -sPTHREAD_POOL_SIZE=4
    )
else()
    find_package(SDL2 REQUIRED)
    find_package(SDL2_image REQUIRED)
    find_package(SDL2_ttf REQUIRED)
    find_package(SDL2_mixer REQUIRED)
    find_package(imgui REQUIRED)
    find_package(OpenGL REQUIRED)
    find_package(GLEW REQUIRED)

    target_include_directories(engine PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui"
    )

    target_sources(engine PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_sdl2.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_opengl3.cpp"
    )

    target_link_libraries(engine
        PUBLIC
            SDL2::SDL2
            SDL2_image::SDL2_image
            imgui::imgui
            sdl_ttf::sdl_ttf
            SDL2_mixer::SDL2_mixer
            OpenGL::GL
            GLEW::GLEW
    )
endif()

endif() # NOT ENGINE_CORE_ONLY

# Engine tests (native only; the web build strips the desktop toolchain). Added
# after both targets exist: core/ tests link engine-core, runtime/ tests link
# engine. enable_testing() is called at the root so ctest finds them.
if(NOT ENGINE_WEB_BUILD)
    add_subdirectory(tests)
endif()
