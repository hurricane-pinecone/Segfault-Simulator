#include <engine/runtime/game/game.h>

#include "engine/core/scripting/luaScripting.h"
#include "engine/runtime/rendering/backend/renderBackends.h"
#include "engine/runtime/rendering/modules/uiModule.h"
#include "engine/runtime/rendering/renderSystem.h"
#include "engine/runtime/rendering/standardScene.h"
#include "engine/runtime/scriptableObjects/scriptableObjects.h"

#include "SDL.h"
#include <SDL_error.h>
#include <SDL_ttf.h>

#include <engine/core/logger/logger.h>
#include <engine/core/util/allocationMetrics.h>
#include <engine/core/util/profiling.h>

#include <algorithm>
#include <cmath>
#include <exception>
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
  // SDL swallows every keystroke on the page, so nothing else on it can be
  // typed into. With this, keys only reach the game when the canvas is focused
  // (click it).
  SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
#endif

  // A window of its own is drawn at the screen's real resolution on a high-DPI
  // display (per monitor, as it moves), not scaled up blurry by Windows.
  if (ownsWindow())
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
  {
    LOG_ERROR(std::string("Error initializing SDL: ") + SDL_GetError());
    return false;
  }
  // No sound (no audio device, a broken driver, a headless machine) is no
  // reason not to play: the game runs silent.
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    LOG_WARN(std::string("No audio, the game runs without sound: ") +
             SDL_GetError());

  if (TTF_Init() != 0)
  {
    LOG_ERROR(std::string("Error initializing TTF: ") + TTF_GetError());
  }

  m_backend = makeRenderBackend();
  if (!m_backend ||
      !m_backend->init("SegFaultSimulator", windowWidth, windowHeight))
  {
    // Another backend may have a driver here (the game's choice may not).
    const std::string failed(m_backend ? m_backend->name() : "");
    if (m_backend)
      m_backend->shutdown();
    m_backend.reset();
    for (const std::string& name : RenderBackends::names())
    {
      if (name == failed)
        continue;
      std::unique_ptr<IRenderBackend> backend = RenderBackends::make(name);
      LOG_INFO("Trying the " + name + " renderer instead");
      if (backend &&
          backend->init("SegFaultSimulator", windowWidth, windowHeight))
      {
        m_backend = std::move(backend);
        break;
      }
      if (backend)
        backend->shutdown();
    }
    if (!m_backend)
    {
      LOG_ERROR("Failed to initialize render backend");
      return false;
    }
  }
  LOG_INFO("Rendering with " + (m_backend->name().empty()
                                    ? std::string("a custom backend")
                                    : std::string(m_backend->name())));
  window = m_backend->window();
  updatePixelSize();

  onInit();

  isRunning = true;

  return true;
}

std::unique_ptr<IRenderBackend> Game::makeRenderBackend()
{
  const std::vector<std::string> names = RenderBackends::names();
  if (names.empty())
  {
    LOG_ERROR("No render backend to run on (register one, e.g. "
              "registerWebGpuBackend())");
    return nullptr;
  }
  return RenderBackends::make(names.front());
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

    self->frame();
  };

  emscripten_set_main_loop(loop, 0, 1);

#else

  while (isRunning)
  {
    if (waitWhileMinimized())
      continue;

    const Uint64 frameStart = SDL_GetPerformanceCounter();
    frame();
    capFrameRate(frameStart);
  }

  destroy();

#endif
}

void Game::frame()
{
  const Uint64 now = SDL_GetPerformanceCounter();
  const double deltaTime =
      std::min(static_cast<double>(now - previousTime) /
                   static_cast<double>(SDL_GetPerformanceFrequency()),
               MAX_FRAME_TIME);
  previousTime = now;

  // A throw escaping a frame (a script's bug, a fatalError()) would otherwise
  // end the program with no word of why: say what it was, and stop.
  try
  {
    processInput();
    update(deltaTime);
    render();
    if (std::exchange(m_collectUnusedAssets, false) && m_services)
      m_services->assetStore.collectUnused();
    if (m_backend && m_backend->deviceLost())
      onFatalError("The graphics device stopped working (it was lost: a "
                   "driver crash or update, or the GPU was removed).");
  }
  catch (const std::exception& error)
  {
    onFatalError(std::string("The game stopped because of an error: ") +
                 error.what());
  }
  catch (...)
  {
    onFatalError("The game stopped because of an unknown error.");
  }
}

void Game::onFatalError(const std::string& message)
{
  LOG_ERROR(message);
#ifndef __EMSCRIPTEN__
  // Works without a window too (e.g. before one opens).
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                           "Error",
                           (message + "\n\nThe log has the details.").c_str(),
                           ownsWindow() ? window : nullptr);
#endif
  isRunning = false;
}

bool Game::waitWhileMinimized()
{
  if (!window || !ownsWindow() ||
      (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) == 0)
    return false;
  // Wake for the next event (left queued: a frame handles it as usual, e.g.
  // the window coming back, or quitting), or now and then to check. The time
  // asleep doesn't count as a frame's.
  const bool event = SDL_WaitEventTimeout(nullptr, 100) == 1;
  previousTime = SDL_GetPerformanceCounter();
  return !event && (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0;
}

void Game::capFrameRate(Uint64 frameStart)
{
  if (m_maxFrameRate <= 0.0)
    return;
  const double frequency = static_cast<double>(SDL_GetPerformanceFrequency());
  const double frameTime = 1.0 / m_maxFrameRate;
  const auto elapsed = [&]
  {
    return static_cast<double>(SDL_GetPerformanceCounter() - frameStart) /
           frequency;
  };
  // Sleep most of what's left (a sleep can overshoot by a millisecond or
  // two), then wait out the rest.
  while (frameTime - elapsed() > 0.002)
    SDL_Delay(1);
  while (elapsed() < frameTime)
  {
  }
}

void Game::setVsync(bool vsync)
{
  if (m_backend)
    m_backend->setVsync(vsync);
}

void Game::updatePixelSize()
{
  m_pixelWidth = windowWidth;
  m_pixelHeight = windowHeight;
  if (window)
    SDL_GetWindowSizeInPixels(window, &m_pixelWidth, &m_pixelHeight);
}

bool Game::devConsoleEnabled() const
{
  const LuaScripting* lua = activeLua();
  return lua && lua->consoleEnabled();
}

Game::SceneView Game::sceneView() const
{
  // In window coordinates, as SDL's mouse events are.
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
  if (!m_services || !m_services->renderer)
  {
    LOG_ERROR("Playing " + scene.generic_string() +
              ": scene files need a renderer, and this game has none");
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
  // What the previous scene loaded and this one doesn't use goes once it has
  // loaded and drawn its first frame (materials load as they're first drawn).
  // Only where the game has the store to itself: a host's (the editor's) is
  // shared with the scenes it edits.
  if (ownsWindow())
  {
    store.beginUseTracking();
    m_collectUnusedAssets = true;
  }
  return replaceScene().play(gameFolder() / scene);
}

void Game::setAssetDatabase(const AssetDatabase* database)
{
  if (m_services)
    m_services->assetStore.setDatabase(database);
}

StandardScene& Game::replaceScene()
{
  sceneManager.destroyAll();
  return *sceneManager.createScene<StandardScene>();
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

  // Mouse positions are relative to where the scene is drawn, in the pixels
  // it's drawn in: a window of the game's own may count its size in points
  // on a high-DPI screen (as SDL's mouse events do), so they're scaled.
  const SceneView view = sceneView();
  const bool mouse = sceneReceivesMouse();
  const glm::vec2 scale =
      ownsWindow() && windowWidth > 0 && windowHeight > 0
          ? glm::vec2(static_cast<float>(m_pixelWidth) / windowWidth,
                      static_cast<float>(m_pixelHeight) / windowHeight)
          : glm::vec2(1.0f);
  const auto setMouse = [&](int x, int y)
  {
    input.mouse().setPosition(
        static_cast<int>(std::lround((x - view.rect.x) * scale.x)),
        static_cast<int>(std::lround((y - view.rect.y) * scale.y)));
  };
  input.mouse().setViewSize(
      static_cast<int>(std::lround(view.rect.w * scale.x)),
      static_cast<int>(std::lround(view.rect.h * scale.y)));

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

    if (sdlEvent.type == SDL_QUIT && onCloseRequested())
    {
      isRunning = false;
    }

    // With ImGui panels dragged out into their own OS windows, SDL only sends
    // SDL_QUIT once the last window closes, so closing the main window must
    // quit explicitly.
    if (sdlEvent.type == SDL_WINDOWEVENT &&
        sdlEvent.window.event == SDL_WINDOWEVENT_CLOSE &&
        sdlEvent.window.windowID == SDL_GetWindowID(window) &&
        onCloseRequested())
    {
      isRunning = false;
    }

    // A new size, or the window moved to a screen of another density: the
    // surface follows, in pixels (never 0 by 0: a window shrunk to nothing
    // keeps its last).
    if (sdlEvent.type == SDL_WINDOWEVENT &&
        (sdlEvent.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
         sdlEvent.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) &&
        sdlEvent.window.windowID == SDL_GetWindowID(window))
    {
      int width = windowWidth, height = windowHeight;
      SDL_GetWindowSize(window, &width, &height);
      if (width > 0 && height > 0)
      {
        windowWidth = width;
        windowHeight = height;
        updatePixelSize();
        if (m_pixelWidth > 0 && m_pixelHeight > 0)
          m_backend->onResize(m_pixelWidth, m_pixelHeight);
      }
    }

    // Only the window the scene is drawn in (not, say, a detached editor
    // panel) gives the game mouse input.
    if (sdlEvent.type == SDL_MOUSEMOTION)
    {
      if (sdlEvent.motion.windowID != view.windowId)
        continue;

      setMouse(sdlEvent.motion.x, sdlEvent.motion.y);
    }

    // A press elsewhere isn't the game's; a release anywhere is (a drag that
    // ends over a detached editor panel mustn't leave the button held).
    const bool button = sdlEvent.type == SDL_MOUSEBUTTONDOWN ||
                        sdlEvent.type == SDL_MOUSEBUTTONUP;
    const bool inView = button && sdlEvent.button.windowID == view.windowId;
    if (sdlEvent.type == SDL_MOUSEBUTTONDOWN && !inView)
      continue;

    if (sdlEvent.type == SDL_MOUSEBUTTONDOWN && mouse)
    {
      setMouse(sdlEvent.button.x, sdlEvent.button.y);
      input.mouse().pressButton(
          static_cast<sfs::MouseButton>(sdlEvent.button.button));
    }

    if (sdlEvent.type == SDL_MOUSEBUTTONUP)
    {
      if (inView)
        setMouse(sdlEvent.button.x, sdlEvent.button.y);
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
      if (sceneReceivesInput() && sceneManager)
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

  // In pixels (the window's size on most screens).
  m_backend->beginFrame(m_pixelWidth, m_pixelHeight);

  // Without a scene the frame still ends (and shows), empty.
  if (sceneManager)
  {
    // The console shows over the scene's UI, where it has one.
    if (auto* render = sceneManager.current()->tryGetSystem<RenderSystem>())
      if (auto* ui = render->module<UiModule>())
        ui->setOverlay(devConsoleEnabled() ? &m_console.layer() : nullptr);

    sceneManager.current()->render();

    {
      ZoneScopedN("Game::onRender");
      sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Render};
      onRender();
    }
  }

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
