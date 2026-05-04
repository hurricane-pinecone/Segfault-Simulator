#pragma once

#include "engine/sceneManager/sceneManager.h"
#include <SDL_events.h>
#include <SDL_render.h>
#include <SDL_stdinc.h>
#include <SDL_video.h>
#include <engine/input/input.h>

#include <engine/assetStore/assetStore.h>
#include <engine/ecs/registry.h>

#include <memory>

namespace sfs
{

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
  virtual void onProcessInput(const Input& input) {};
  virtual void onUpdate(double deltaTime) {};
  virtual void onRender() {};
  virtual void onDestroy() = 0;

protected:
  SceneManager sceneManager;
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
  Input input;
};

} // namespace sfs
