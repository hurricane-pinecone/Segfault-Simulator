#include <engine/runtime/game/game.h>

#include "engine/core/scripting/luaScripting.h"
#include "engine/runtime/rendering/backend/glRenderBackend.h"
#include "engine/runtime/rendering/gl/openGLQuadRenderer.h"
#include "engine/runtime/rendering3d/scene3D.h"
#include "engine/runtime/scriptableObjects/scriptableObjects.h"

#include "SDL.h"
#include <SDL_error.h>
#include <SDL_ttf.h>

#include <engine/core/logger/logger.h>
#include <engine/core/util/allocationMetrics.h>
#include <engine/core/util/profiling.h>

#include <memory>
#include <string>
#include <utility>

#include "engine/runtime/rendering/debug/ui.h"

#ifdef __EMSCRIPTEN__
  #include <emscripten.h>
#endif

namespace sfs
{

bool Game::init(int windowWidth, int windowHeight)
{
  this->windowWidth = windowWidth;
  this->windowHeight = windowHeight;

#ifdef ENGINE_WEB
  // Scope keyboard input to the canvas (default is the whole window). Otherwise
  // SDL swallows every keystroke, so the on-page Lua editor can't be typed into
  // and WASD drives the player even while editing. With this, keys only reach
  // the game when the canvas is focused (click it); the editor gets them when
  // it's focused.
  SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
#endif

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0)
  {
    LOG_ERROR(std::string("Error initializing SDL: ") + SDL_GetError());
    return false;
  }

  if (TTF_Init() != 0)
  {
    LOG_ERROR(std::string("Error initializing TTF: ") + TTF_GetError());
  }

  m_backend = makeRenderBackend();
  if (!m_backend->init("SegFaultSimulator", windowWidth, windowHeight))
  {
    LOG_ERROR("Failed to initialize render backend");
    return false;
  }
  window = m_backend->window();

  onInit();

  isRunning = true;

  return true;
}

std::unique_ptr<IQuadRenderer> Game::createQuadRenderer(int windowWidth,
                                                        int windowHeight)
{
  return std::make_unique<OpenGLQuadRenderer>(windowWidth, windowHeight);
}

std::unique_ptr<IRenderBackend> Game::makeRenderBackend()
{
  return std::make_unique<GLRenderBackend>(
      [this](int w, int h) { return createQuadRenderer(w, h); });
}

void Game::setup()
{
  sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Setup};

  m_services = m_backend->sceneServices();
  sceneManager.setSceneServices(m_services);
  sceneManager.setGame(this);

  onSetup();
}

void Game::run()
{
  previousTime = SDL_GetPerformanceCounter();

#ifdef __EMSCRIPTEN__

  // Store this pointer for the static loop
  static Game* self = this;

  auto loop = []()
  {
    if (!self->isRunning)
    {
      self->destroy();
      emscripten_cancel_main_loop(); // stop browser loop
      return;
    }

    Uint64 now = SDL_GetPerformanceCounter();
    double deltaTime = static_cast<double>(now - self->previousTime) /
                       SDL_GetPerformanceFrequency();

    self->previousTime = now;

    self->processInput();
    self->update(deltaTime);
    self->render();
  };

  emscripten_set_main_loop(loop, 0, 1);

#else

  while (isRunning)
  {
    Uint64 now = SDL_GetPerformanceCounter();
    double deltaTime =
        static_cast<double>(now - previousTime) / SDL_GetPerformanceFrequency();

    previousTime = now;

    processInput();
    update(deltaTime);
    render();
  }

  destroy();

#endif
}

bool Game::devConsoleEnabled() const
{
  const LuaScripting* lua = activeLua();
  return lua && lua->consoleEnabled();
}

Game::SceneView Game::sceneView() const
{
  return SceneView{window ? SDL_GetWindowID(window) : 0,
                   SDL_Rect{0, 0, windowWidth, windowHeight}};
}

void Game::onLoadScene(const std::filesystem::path& scene)
{
  if (!playScene(scene))
    LOG_ERROR("loadScene: couldn't play " + scene.generic_string());
}

bool Game::playScene(const std::filesystem::path& scene)
{
  if (!m_services || !m_services->renderer3D)
  {
    LOG_ERROR("Playing " + scene.generic_string() +
              ": scene files need a 3D renderer, and this game has none");
    return false;
  }
  AssetStore& store = m_services->assetStore;
  if (!store.database())
  {
    std::error_code error;
    bool any = false;
    for (const std::filesystem::path& folder : AssetDatabase::projectFolders())
      any |= std::filesystem::is_directory(gameFolder() / folder, error);
    if (any)
      store.setDatabase(
          &m_assetDatabase.emplace(gameFolder(),
                                   AssetDatabase::Mode::ReadOnly,
                                   AssetDatabase::projectFolders()));
  }
  // Scriptable objects come from the same catalogue.
  scriptableObjects().setCatalogue(store.database());
  return replaceScene().play(gameFolder() / scene);
}

void Game::setAssetDatabase(const AssetDatabase* database)
{
  if (m_services)
    m_services->assetStore.setDatabase(database);
}

Scene3D& Game::replaceScene()
{
  sceneManager.destroyAll();
  return *sceneManager.createScene<Scene3D>();
}

void Game::applyRequests()
{
  if (m_requestedScene)
  {
    const std::filesystem::path scene =
        *std::exchange(m_requestedScene, std::nullopt);
    onLoadScene(scene);
  }
  if (std::exchange(m_quitRequested, false))
    onQuit();
}

bool Game::canChangeWindow()
{
  if (ownsWindow() && window)
    return true;
  if (!m_windowChangeLogged)
    LOG_INFO("The game changed its window (title, size or fullscreen); that's "
             "ignored in the "
             "editor and applies in the exported game");
  m_windowChangeLogged = true;
  return false;
}

void Game::setWindowTitle(const std::string& title)
{
  if (canChangeWindow())
    SDL_SetWindowTitle(window, title.c_str());
}

void Game::setFullscreen(bool fullscreen)
{
  if (canChangeWindow())
    SDL_SetWindowFullscreen(
        window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

bool Game::isFullscreen() const
{
  return window && ownsWindow() &&
         (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
}

void Game::setWindowSize(int width, int height)
{
  if (width <= 0 || height <= 0)
    return;
  if (canChangeWindow())
    SDL_SetWindowSize(
        window, width, height); // the resize event updates the backend
}

void Game::processInput()
{
  ZoneScopedN("Game::processInput");

  input.mouse().beginFrame();
  input.keyboard().beginFrame();

  // Mouse positions are relative to where the scene is drawn.
  const SceneView view = sceneView();
  const bool mouse = sceneReceivesMouse();
  input.mouse().setViewSize(view.rect.w, view.rect.h);

  SDL_Event sdlEvent;
  while (SDL_PollEvent(&sdlEvent))
  {

    // The console gets first refusal on input: while open it owns the keyboard
    // so typing a command doesn't leak through to ImGui or the game. Available
    // on web too -- it shares the VM the on-page editor drives.
    if (devConsoleEnabled() && m_console.handleEvent(sdlEvent))
      continue;

    m_backend->imguiProcessEvent(sdlEvent);
    onEvent(sdlEvent);

    if (sdlEvent.type == SDL_QUIT)
    {
      isRunning = false;
    }

    // With ImGui panels dragged out into their own OS windows, SDL only sends
    // SDL_QUIT once the last window closes, so closing the main window must
    // quit explicitly.
    if (sdlEvent.type == SDL_WINDOWEVENT &&
        sdlEvent.window.event == SDL_WINDOWEVENT_CLOSE &&
        sdlEvent.window.windowID == SDL_GetWindowID(window))
    {
      isRunning = false;
    }

    if (sdlEvent.type == SDL_WINDOWEVENT &&
        sdlEvent.window.event == SDL_WINDOWEVENT_SIZE_CHANGED &&
        sdlEvent.window.windowID == SDL_GetWindowID(window))
    {
      windowWidth = sdlEvent.window.data1;
      windowHeight = sdlEvent.window.data2;
      m_backend->onResize(windowWidth, windowHeight);
    }

    // Only the window the scene is drawn in (not, say, a detached editor
    // panel) gives the game mouse input.
    if (sdlEvent.type == SDL_MOUSEMOTION)
    {
      if (sdlEvent.motion.windowID != view.windowId)
        continue;

      input.mouse().setPosition(
          sdlEvent.motion.x - view.rect.x, sdlEvent.motion.y - view.rect.y);
    }

    if ((sdlEvent.type == SDL_MOUSEBUTTONDOWN ||
         sdlEvent.type == SDL_MOUSEBUTTONUP) &&
        sdlEvent.button.windowID != view.windowId)
      continue;

    if (sdlEvent.type == SDL_MOUSEBUTTONDOWN && mouse)
    {
      input.mouse().setPosition(
          sdlEvent.button.x - view.rect.x, sdlEvent.button.y - view.rect.y);
      input.mouse().pressButton(
          static_cast<sfs::MouseButton>(sdlEvent.button.button));
    }

    if (sdlEvent.type == SDL_MOUSEBUTTONUP)
    {
      input.mouse().setPosition(
          sdlEvent.button.x - view.rect.x, sdlEvent.button.y - view.rect.y);
      input.mouse().releaseButton(
          static_cast<sfs::MouseButton>(sdlEvent.button.button));
    }

    if (sdlEvent.type == SDL_MOUSEWHEEL && mouse)
    {
      input.mouse().addScroll(sdlEvent.wheel.x, sdlEvent.wheel.y);
    }

    // Seen as events too, so a tap shorter than a frame still registers.
    if (sdlEvent.type == SDL_KEYDOWN && !sdlEvent.key.repeat)
      input.keyboard().pressKey(sdlEvent.key.keysym.scancode);
  }

  // input.update contains SDL stuff which we don't want to track
  input.update();
  {
    sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Input};

    // While the console is open it has keyboard focus; the game must not also
    // act on the keys being typed (input is polled, not event-driven).
    if (!(devConsoleEnabled() && m_console.isOpen()))
    {
      if (sceneReceivesInput())
        sceneManager.current()->processInput(input);
      onProcessInput(input);
    }
  }
}

void Game::update(double deltaTime)
{
  ZoneScopedN("Game::update");

  sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Update};
  if (sceneManager)
  {
    sceneManager.current()->update(deltaTime);
    onUpdate(deltaTime);
  }
  // After every script has run, so none is destroyed mid-update.
  applyRequests();
}

void Game::render()
{
  ZoneScopedN("Game::render");

  m_backend->beginFrame(windowWidth, windowHeight);

  {
    if (!sceneManager)
      return;

    sceneManager.current()->render();

    {
      ZoneScopedN("Game::onRender");
      sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Render};
      onRender();
    }
  }

  // The console draws through the 2D quad + text renderers, so it only
  // appears on a backend that has them.
  if (devConsoleEnabled() && m_services && m_services->textRenderer &&
      m_services->quadRenderer)
    m_console.render(*m_services->textRenderer,
                     *m_services->quadRenderer,
                     m_services->assetStore,
                     windowWidth,
                     windowHeight);

#if !defined(NDEBUG) && !defined(ENGINE_WEB)
  if (m_debugUiVisible && m_backend->imguiAvailable())
    renderDebugUI(*m_backend, sceneManager.current(), [this] { onDebugUI(); });
#endif

  m_backend->endFrame();
}

void Game::destroy()
{
  onDestroy();

  // Loaded scriptable objects first: they may be of the game module's types.
  scriptableObjects().setCatalogue(nullptr);

  // Scenes first: their systems may own GPU resources (meshes, textures) that
  // must be released before the backend tears down the device.
  sceneManager.destroyAll();

  // Tear down GPU-backed resources while the context is still valid.
  if (m_backend)
  {
    m_backend->shutdown();
    m_backend.reset();
  }
  window = nullptr;

  SDL_Quit();
}

} // namespace sfs
