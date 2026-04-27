#pragma once

#include "SDL_render.h"
#include "SDL_stdinc.h"
#include "SDL_video.h"

#include "assetStore/assetStore.h"
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

  // SDL cleans these up
  SDL_Window* window;
  SDL_Renderer* renderer;

  std::unique_ptr<Registry> registry;
  std::unique_ptr<AssetStore> assetStore;

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
