# Builds this project's scripts (scripts/) into its game module, the
# library the editor loads to play the game. Open the folder in any
# CMake-aware IDE to work on scripts outside the editor too.
cmake_minimum_required(VERSION 3.24)
project(Neon_Sprint LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(engine REQUIRED)

sfs_add_game_module(Neon_Sprint)
