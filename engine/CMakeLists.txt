file(GLOB_RECURSE ENGINE_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

if(EMSCRIPTEN)
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

if(EMSCRIPTEN)
    target_compile_definitions(engine PUBLIC
        ENGINE_WEB=1
    )

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
    )
else()
    find_package(lua REQUIRED)
    find_package(SDL2 REQUIRED)
    find_package(SDL2_image REQUIRED)
    find_package(SDL2_ttf REQUIRED)
    find_package(SDL2_mixer REQUIRED)
    find_package(imgui REQUIRED)

    target_include_directories(engine PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends"
    )

    target_sources(engine PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_sdl2.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_sdlrenderer2.cpp"
    )

    target_link_libraries(engine
        PUBLIC
            SDL2::SDL2
            SDL2_image::SDL2_image
            imgui::imgui
            lua::lua
            sdl_ttf::sdl_ttf
            SDL2_mixer::SDL2_mixer
    )
endif()
