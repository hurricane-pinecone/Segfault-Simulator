#!/bin/bash

cmake --build --preset conan-debug || exit 1
leaks --atExit -- ./build/Debug/bin/GameEngine
