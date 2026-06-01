set(ENGINE_WEB_BUILD FALSE)

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR EMSCRIPTEN)
    set(ENGINE_WEB_BUILD TRUE)
endif()

message(STATUS "ENGINE_WEB_BUILD=${ENGINE_WEB_BUILD}")
message(STATUS "CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}")
message(STATUS "EMSCRIPTEN=${EMSCRIPTEN}")

file(GLOB_RECURSE ENGINE_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

if(ENGINE_WEB_BUILD)
    list(FILTER ENGINE_SRC EXCLUDE REGEX ".*/imgui/.*")
    list(FILTER ENGINE_SRC EXCLUDE REGEX ".*/lua/.*")
endif()

add_library(engine STATIC
    ${ENGINE_SRC}
    ${ENGINE_HEADERS}
)

target_include_directories(engine
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/glm"
    PRIVATE
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
    target_compile_definitions(engine PUBLIC
        ENGINE_WEB=1
    )

    target_compile_options(engine PUBLIC
        -sUSE_SDL=2
        -sUSE_SDL_IMAGE=2
        -sSDL2_IMAGE_FORMATS=["png"]
        -sUSE_SDL_TTF=2
        -sUSE_SDL_MIXER=2
        -pthread
        -matomics
        -mbulk-memory
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
    find_package(Threads REQUIRED)
    find_package(lua REQUIRED)
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
            lua::lua
            sdl_ttf::sdl_ttf
            SDL2_mixer::SDL2_mixer
            OpenGL::GL
            GLEW::GLEW
            Threads::Threads
    )
endif()
