SRC := $(shell find src -name '*.cpp')

build:
	clang++ -Wall -Wno-unknown-warning-option -std=c++17 \
	-I./src \
	-I./lib \
	-I./lib/SDL2/include \
	-I./lib/imgui \
	-I./lib/imgui/backends/ \
	$(SRC) \
	lib/imgui/*.cpp \
	lib/imgui/backends/imgui_impl_sdl2.cpp \
	-L./lib/SDL2/lib \
	-lSDL2 -lSDL2_image -lSDL2_ttf -lSDL2_mixer \
	-llua \
	-o bin/GameEngine

run:
	./bin/GameEngine

clean:
	rm ./bin/GameEngine
