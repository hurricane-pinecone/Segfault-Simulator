#pragma once

#include "engine/runtime/TextRenderer/textRenderer.h"
#include "engine/runtime/rendering/iQuadRenderer.h"
#include "engine/runtime/sceneManager/sceneManager.h"
#include <SDL_events.h>
#include <SDL_render.h>
#include <SDL_stdinc.h>
#include <SDL_video.h>
#include <engine/runtime/input/input.h>

#include <engine/runtime/assetStore/assetStore.h>
#include <engine/core/ecs/registry.h>

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

  bool init(int windowWidth, int windowHeight);
  void setup();
  void run();
  void tick();
  void destroy();

protected:
  virtual void onInit() {};
  virtual void onSetup() {};
  virtual void onProcessInput(const Input& input) {};
  virtual void onUpdate(double deltaTime) {};
  virtual void onRender() {};
  // App-level debug UI, drawn inside the engine's ImGui frame (native debug
  // builds only) alongside the per-scene debug UI.
  virtual void onDebugUI() {};
  virtual void onDestroy() = 0;

  /**
   * Create the quad-rendering backend the game draws through. The default backs
   * the core 2D renderer; a game needing the isometric heightfield path
   * overrides this to return an isometric backend.
   *
   * @param windowWidth  viewport width in pixels
   * @param windowHeight viewport height in pixels
   * @return owning handle to the renderer
   */
  virtual std::unique_ptr<IQuadRenderer> createQuadRenderer(int windowWidth,
                                                            int windowHeight);

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
  SDL_GLContext m_glContext;

  std::unique_ptr<IQuadRenderer> m_quadRenderer;
  std::unique_ptr<TextRenderer> m_textRenderer;
};

} // namespace sfs
