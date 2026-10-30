#pragma once

#include "engine/runtime/console/devConsole.h"
#include "engine/runtime/rendering/backend/iRenderBackend.h"
#include "engine/runtime/sceneManager/sceneManager.h"
#include <SDL_events.h>
#include <SDL_render.h>
#include <SDL_stdinc.h>
#include <SDL_video.h>
#include <engine/runtime/input/input.h>

#include <engine/core/ecs/registry.h>
#include <engine/runtime/assetStore/assetStore.h>

#include "glm/glm/ext/vector_int2.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace sfs
{

class Scene3D;

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

  // --- For game code (scripts reach the game through Scene::game()) --------

  /** End the game once this frame is done. In the editor, this stops Play. */
  void quit() { m_quitRequested = true; }

  /**
   * Switch to another scene file once this frame is done, e.g.
   * "scenes/level2.scene" (relative to the game's folder: the project in the
   * editor, the export folder in a shipped game). The current scene and its
   * scripts are destroyed, then the new one's scripts start.
   */
  void loadScene(const std::filesystem::path& scene)
  {
    m_requestedScene = scene;
  }

  // The game's window. Inside the editor the window is the editor's, so these
  // are ignored there (logged once).
  void setWindowTitle(const std::string& title);
  void setFullscreen(bool fullscreen);
  bool isFullscreen() const;
  void setWindowSize(int width, int height);
  glm::ivec2 windowSize() const { return {windowWidth, windowHeight}; }

  /**
   * Where the current scene is drawn: an OS window (by SDL window id) and a
   * rectangle in it, in window pixels. Mouse positions given to the scene are
   * relative to the rectangle's top-left corner.
   */
  struct SceneView
  {
    Uint32 windowId = 0;
    SDL_Rect rect{0, 0, 0, 0};
  };

protected:
  virtual void onInit(){};
  virtual void onSetup(){};
  virtual void onProcessInput(const Input& input){};
  // Every SDL event, as it's polled (before the game's own input handling),
  // e.g. for files dropped onto the window.
  virtual void onEvent(const SDL_Event& event){};
  virtual void onUpdate(double deltaTime){};
  virtual void onRender(){};
  // App-level debug UI, drawn inside the engine's ImGui frame (native debug
  // builds only) alongside the per-scene debug UI.
  virtual void onDebugUI(){};
  virtual void onDestroy() = 0;

  /**
   * Create the render backend the game runs on. The default is the first one
   * the program offers (RenderBackends: a player registers the engine's, see
   * registerWebGpuBackend()); a game can return any backend of its own.
   */
  virtual std::unique_ptr<IRenderBackend> makeRenderBackend();

  /**
   * Whether the current scene gets this frame's input. Always, by default; a
   * host that embeds the game (the editor) passes it on only while the game's
   * view has focus.
   */
  virtual bool sceneReceivesInput() const { return true; }

  /**
   * Whether the scene gets mouse presses and the wheel (on top of
   * sceneReceivesInput()). Always, by default; the editor passes them on only
   * while the pointer is over the game's view, so scrolling another panel
   * doesn't zoom the game's camera. Releases always get through, so a drag
   * that ends outside doesn't leave a button held.
   */
  virtual bool sceneReceivesMouse() const { return true; }

  /**
   * Where the current scene is drawn. The whole window, by default; a host that
   * draws it into part of its UI (the editor's viewport) says where.
   */
  virtual SceneView sceneView() const;

  /** Carry out quit(). By default the game loop ends. */
  virtual void onQuit() { isRunning = false; }

  /**
   * Carry out loadScene(), between frames. By default, playScene() (logging a
   * failure); a host overrides it to handle failure its own way.
   */
  virtual void onLoadScene(const std::filesystem::path& scene);

  /**
   * Replace the current scene with a scene file (relative to gameFolder()) and
   * start it: a fresh scene from replaceScene(), then Scene3D::play. Needs a
   * 3D renderer.
   *
   * @return false (logged) if it can't
   */
  bool playScene(const std::filesystem::path& scene);

  /** The folder scene file paths are relative to (the game's folder). */
  virtual std::filesystem::path gameFolder() const
  {
    return std::filesystem::current_path();
  }

  /**
   * The asset catalogue scenes load assets by id from (null for none). Without
   * one, playScene() opens gameFolder()/assets read-only, as a shipped game
   * needs; a host with its own (the editor's read-write one) sets it here.
   */
  void setAssetDatabase(const AssetDatabase* database);

  /**
   * Destroy the current scene and create the new, current one a scene file is
   * played into. A plain Scene3D by default; a host with its own kind of scene
   * (the editor) makes that instead.
   */
  virtual Scene3D& replaceScene();

  /** Whether the window belongs to the game (not when it runs in the editor).
   */
  virtual bool ownsWindow() const { return true; }

  // The active render backend, for a game that needs its concrete type (e.g. to
  // hand a WebGPU device to a scene's systems).
  IRenderBackend* renderBackend() const { return m_backend.get(); }

protected:
  SceneManager sceneManager;
  bool isRunning = false;
  int windowWidth;
  int windowHeight;
  // Owned by the render backend; cached here for input/event routing.
  SDL_Window* window = nullptr;

private:
  void processInput();
  void update(double deltaTime);
  void render();

  // Act on quit() and loadScene() calls, after the frame's updates.
  void applyRequests();

  // True (logging once) if game code may change the window.
  bool canChangeWindow();

  // Whether the active Lua VM (if any) has opted into the dev console.
  bool devConsoleEnabled() const;

private:
  Uint64 previousTime;
  Input input;

  bool m_debugUiVisible = true;

  bool m_quitRequested = false;
  std::optional<std::filesystem::path> m_requestedScene;
  bool m_windowChangeLogged = false;

  std::unique_ptr<IRenderBackend> m_backend;
  // Drawing/asset services the active backend supplies; non-owning.
  SceneServices* m_services = nullptr;
  // The shipped game's own assets/ catalogue, opened by playScene().
  std::optional<AssetDatabase> m_assetDatabase;

  // Backtick-toggled Lua console, drawn over the frame (native builds only).
  DevConsole m_console;
};

} // namespace sfs
