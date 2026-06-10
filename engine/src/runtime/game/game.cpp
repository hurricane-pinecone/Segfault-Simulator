#include <engine/runtime/game/game.h>

#include "engine/core/scripting/luaScripting.h"
#include "engine/runtime/rendering/backend/glRenderBackend.h"
#include "engine/runtime/rendering/gl/openGLQuadRenderer.h"

#include "SDL.h"
#include <SDL_error.h>
#include <SDL_ttf.h>

#include <engine/core/logger/logger.h>
#include <engine/core/util/allocationMetrics.h>
#include <engine/core/util/profiling.h>

#include <memory>
#include <string>

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

void Game::processInput()
{
  ZoneScopedN("Game::processInput");

  input.mouse().beginFrame();

  SDL_Event sdlEvent;
  while (SDL_PollEvent(&sdlEvent))
  {

    // The console gets first refusal on input: while open it owns the keyboard
    // so typing a command doesn't leak through to ImGui or the game. Available
    // on web too -- it shares the VM the on-page editor drives.
    if (devConsoleEnabled() && m_console.handleEvent(sdlEvent))
      continue;

    m_backend->imguiProcessEvent(sdlEvent);

    if (sdlEvent.type == SDL_QUIT)
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

    if (sdlEvent.type == SDL_MOUSEMOTION)
    {
      if (sdlEvent.motion.windowID != SDL_GetWindowID(window))
        continue;

      if (sdlEvent.motion.x < 0 || sdlEvent.motion.y < 0 ||
          sdlEvent.motion.x >= windowWidth || sdlEvent.motion.y >= windowHeight)
        continue;

      input.mouse().setPosition(sdlEvent.motion.x, sdlEvent.motion.y);
      // input.mouse().processDrag();
    }

    if (sdlEvent.type == SDL_MOUSEBUTTONDOWN)
    {
      input.mouse().setPosition(sdlEvent.button.x, sdlEvent.button.y);
      input.mouse().pressButton(
          static_cast<sfs::MouseButton>(sdlEvent.button.button));
    }

    if (sdlEvent.type == SDL_MOUSEBUTTONUP)
    {
      input.mouse().setPosition(sdlEvent.button.x, sdlEvent.button.y);
      input.mouse().releaseButton(
          static_cast<sfs::MouseButton>(sdlEvent.button.button));
    }

    if (sdlEvent.type == SDL_MOUSEWHEEL)
    {
      input.mouse().addScroll(sdlEvent.wheel.x, sdlEvent.wheel.y);
    }
  }

  // input.update contains SDL stuff which we don't want to track
  input.update();
  {
    sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Input};

    // While the console is open it has keyboard focus; the game must not also
    // act on the keys being typed (input is polled, not event-driven).
    if (!(devConsoleEnabled() && m_console.isOpen()))
    {
      sceneManager.current()->processInput(input);
      onProcessInput(input);
    }
  }
}

void Game::update(double deltaTime)
{
  ZoneScopedN("Game::update");

  sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Update};
  if (!sceneManager)
    return;
  sceneManager.current()->update(deltaTime);
  onUpdate(deltaTime);
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

  if (devConsoleEnabled() && m_services)
    m_console.render(m_services->textRenderer,
                     m_services->quadRenderer,
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
