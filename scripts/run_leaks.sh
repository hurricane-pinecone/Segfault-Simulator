#!/bin/bash

cmake --build --preset debug || exit 1

cd ./build/Debug/bin || exit 1
leaks --atExit -- ./game
