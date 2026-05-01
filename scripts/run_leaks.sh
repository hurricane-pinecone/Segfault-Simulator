#!/bin/bash

cmake --build --preset conan-debug || exit 1
(cd ./build/Debug/bin && leaks --atExit -- ./GameEngine)
