#pragma once

#include <SDL_events.h>
#include <SDL_render.h>
#include <SDL_stdinc.h>
#include <SDL_video.h>

#include <engine/assetStore/assetStore.h>
#include <engine/ecs/registry.h>

#include <memory>

const double TARGET_FPS = 165.0;
const double FRAME_TIME = 1.0 / TARGET_FPS;

class Game
{
public:
  Game() = default;
  virtual ~Game() = default;

  void init(int windowWidth, int windowHeight);
  void setup();
  void run();
  void destroy();

protected:
  virtual void onInit() {};
  virtual void onSetup() {};
  virtual void onProcessInput(SDL_Event& event) {};
  virtual void onUpdate(double deltaTime) {};
  virtual void onRender() {};
  virtual void onDestroy() = 0;

protected:
  std::unique_ptr<Registry> registry;
  std::unique_ptr<AssetStore> assetStore;
  bool isRunning = false;
  int windowWidth;
  int windowHeight;
  // SDL cleans these up
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;

private:
  void processInput();
  void update(double deltaTime);
  void render();

private:
  Uint64 previousTime;
};
