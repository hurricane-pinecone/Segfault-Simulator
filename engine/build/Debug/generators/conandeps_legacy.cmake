message(STATUS "Conan: Using CMakeDeps conandeps_legacy.cmake aggregator via include()")
message(STATUS "Conan: It is recommended to use explicit find_package() per dependency instead")

find_package(lua)
find_package(SDL2_image)
find_package(SDL2_ttf)
find_package(SDL2_mixer)
find_package(SDL2)
find_package(imgui)
find_package(glm)

set(CONANDEPS_LEGACY  lua::lua  SDL2_image::SDL2_image  sdl_ttf::sdl_ttf  SDL2_mixer::SDL2_mixer  SDL2::SDL2main  imgui::imgui  glm::glm )