#pragma once

#include "SDL_render.h"
#include "SDL_stdinc.h"
#include "SDL_video.h"
#include "ecs/registry.h"
#include <memory>

const double TARGET_FPS = 165.0;
const double FRAME_TIME = 1.0 / TARGET_FPS;

class Game
{
private:
  bool isRunning;
  int windowWidth;
  int windowHeight;
  Uint64 previousTime;

  SDL_Window* window;
  SDL_Renderer* renderer;

  std::unique_ptr<Registry> registry;

public:
  Game();
  ~Game();

  void init();
  void setup();
  void run();
  void processInput();
  void update(double deltaTime);
  void render();
  void destroy();
};
