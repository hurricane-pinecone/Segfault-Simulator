#pragma once

#include "engine/runtime/TextRenderer/textRenderer.h"
#include "engine/runtime/console/devConsole.h"
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

  // Show/hide the debug overlay (stats, engine render controls, and the
  // per-scene / app debug panels). Native debug builds only -- the overlay is
  // compiled out elsewhere, so these are harmless no-ops there. Intended to be
  // driven live (e.g. a Lua command or a key).
  void setDebugUiVisible(bool visible) { m_debugUiVisible = visible; }
  void toggleDebugUi() { m_debugUiVisible = !m_debugUiVisible; }
  bool isDebugUiVisible() const { return m_debugUiVisible; }

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

  // Whether the active Lua VM (if any) has opted into the dev console.
  bool devConsoleEnabled() const;

private:
  Uint64 previousTime;
  Input input;
  SDL_GLContext m_glContext;

  bool m_debugUiVisible = true;

  std::unique_ptr<IQuadRenderer> m_quadRenderer;
  std::unique_ptr<TextRenderer> m_textRenderer;

  // Backtick-toggled Lua console, drawn over the frame (native builds only).
  DevConsole m_console;
};

} // namespace sfs
