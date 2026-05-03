file(GLOB_RECURSE ENGINE_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

add_library(engine STATIC
    ${ENGINE_SRC}
    ${ENGINE_HEADERS}
)

target_include_directories(engine
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib"
)

target_compile_options(engine PRIVATE
    -Wall
    -Wno-unknown-warning-option
)

find_package(lua REQUIRED)
find_package(SDL2 REQUIRED)
find_package(SDL2_image REQUIRED)
find_package(SDL2_ttf REQUIRED)
find_package(SDL2_mixer REQUIRED)
find_package(imgui REQUIRED)
find_package(glm REQUIRED)
find_package(nlohmann_json REQUIRED)

target_sources(engine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_sdl2.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_sdlrenderer2.cpp"
)

target_link_libraries(engine
    PUBLIC
        glm::glm
        SDL2::SDL2
        SDL2_image::SDL2_image
        imgui::imgui
        lua::lua
        sdl_ttf::sdl_ttf
        SDL2_mixer::SDL2_mixer
    PRIVATE
        nlohmann_json::nlohmann_json
)
