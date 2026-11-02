// Windows only. The MinGW build links SDL2main's WinMain into every program
// (-Wl,-u,WinMain, which games and the editor need), and WinMain calls
// SDL_main. The tests are console programs with their own main() (see
// SDL_MAIN_HANDLED in CMakeLists.txt), so WinMain never runs, but the linker
// still needs an SDL_main to find.
extern "C" int SDL_main(int, char**) { return 1; }
