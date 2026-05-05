
#include <SDL_error.h>
#include <engine/game/game.h>

#include "SDL.h"

#include <engine/assetStore/sprite.h>
#include <engine/components/rigidBodyComponent.h>
#include <engine/components/spriteComponent.h>
#include <engine/components/transformComponent.h>
#include <engine/game/game.h>
#include <engine/logger/logger.h>
#include <engine/mapLoader/mapLoader.h>
#include <engine/systems/movementSystem.h>
#include <engine/systems/renderSystem.h>
#include <engine/utils/allocationMetrics.h>
#include <engine/utils/ui.h>
#include <glm/glm/ext/vector_float2.hpp>

#include <glm/glm/ext/vector_float3.hpp>
#include <memory>
#include <string>

#ifndef ENGINE_WEB
  #include "imgui.h"
  #include "imgui_impl_sdl2.h"
  #include "imgui_impl_sdlrenderer2.h"
#endif

#ifdef __EMSCRIPTEN__
  #include <emscripten.h>
#endif

namespace sfs
{

bool Game::init(int windowWidth, int windowHeight)
{
  this->windowWidth = windowWidth;
  this->windowHeight = windowHeight;

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0)
  {
    LOG_ERROR(std::string("Error initializing SDL: ") + SDL_GetError());
    return false;
  }

  SDL_DisplayMode displayMode;
  SDL_GetCurrentDisplayMode(0, &displayMode);

  window = SDL_CreateWindow("SegFaultSimulator",
                            SDL_WINDOWPOS_CENTERED,
                            SDL_WINDOWPOS_CENTERED,
                            windowWidth,
                            windowHeight,
                            SDL_WINDOW_BORDERLESS);

  if (!window)
  {
    LOG_ERROR("Error creating window");
    return false;
  }

  renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

  if (!renderer)
  {
    LOG_INFO(std::string("Error creating renderer: ") + SDL_GetError());
    return false;
  }

  // SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);

#ifndef ENGINE_WEB
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);
#endif

  LOG_INFO("Game renderer ptr: " +
           std::to_string(reinterpret_cast<std::uintptr_t>(renderer)));

  onInit();

  isRunning = true;

  return true;
}

void Game::setup()
{
  sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Setup};

  assetStore = std::make_unique<AssetStore>(*renderer);
  sceneManager.setAssetStore(assetStore.get());

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

void Game::processInput()
{
  input.mouse().beginFrame();

  SDL_Event sdlEvent;
  while (SDL_PollEvent(&sdlEvent))
  {

#ifndef ENGINE_WEB
    ImGui_ImplSDL2_ProcessEvent(&sdlEvent);
#endif

    if (sdlEvent.type == SDL_QUIT)
    {
      isRunning = false;
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

    sceneManager.current()->processInput(input);
    onProcessInput(input);
  }
}

void Game::update(double deltaTime)
{
  sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Update};
  if (!sceneManager)
    return;
  sceneManager.current()->update(deltaTime);
  onUpdate(deltaTime);
}

void Game::render()
{
  // Render background
  SDL_SetRenderDrawColor(renderer, 21, 21, 21, 255);
  SDL_RenderClear(renderer);

  {
    sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Render};

    if (!sceneManager)
      return;

    sceneManager.current()->render(*renderer);

    onRender();
  }

#if !defined(NDEBUG) && !defined(ENGINE_WEB)
  renderDebugUI(renderer);
#endif

  SDL_RenderPresent(renderer);
}

void Game::destroy()
{
  onDestroy();

#ifndef ENGINE_WEB
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
#endif

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
}

} // namespace sfs
