#include <engine/game/game.h>

#include "SDL.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
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
#include <glm/ext/vector_float2.hpp>

#include <glm/ext/vector_float3.hpp>
#include <memory>
#include <string>

namespace sfs
{

void Game::init(int windowWidth, int windowHeight)
{
  this->windowWidth = windowWidth;
  this->windowHeight = windowHeight;

  if (SDL_Init(SDL_INIT_EVERYTHING) != 0)
  {
    LOG_ERROR("Error initializing SDL");
    return;
  }

  SDL_DisplayMode displayMode;
  SDL_GetCurrentDisplayMode(0, &displayMode);

  window = SDL_CreateWindow(NULL,
                            SDL_WINDOWPOS_CENTERED,
                            SDL_WINDOWPOS_CENTERED,
                            windowWidth,
                            windowHeight,
                            SDL_WINDOW_BORDERLESS);

  if (!window)
  {
    LOG_ERROR("Error creating window");
    return;
  }

  renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

  if (!renderer)
  {
    LOG_ERROR("Error creating renderer");
    return;
  }

  // SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);

  onInit();

  isRunning = true;
}

void Game::setup()
{
  sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Setup};

  registry = std::make_unique<Registry>();
  assetStore = std::make_unique<AssetStore>(*renderer);

  registry->addSystem<MovementSystem>();
  registry->addSystem<RenderSystem>(*assetStore, windowWidth, windowHeight);

  onSetup();
}

void Game::run()
{
  previousTime = SDL_GetPerformanceCounter();

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
}

void Game::processInput()
{
  input.mouse().beginFrame();

  SDL_Event sdlEvent;
  while (SDL_PollEvent(&sdlEvent))
  {
    ImGui_ImplSDL2_ProcessEvent(&sdlEvent);

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

    onProcessInput(input);
  }
}

void Game::update(double deltaTime)
{
  sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Update};
  registry->update(deltaTime);
  onUpdate(deltaTime);
}

void Game::render()
{
  // Render background
  SDL_SetRenderDrawColor(renderer, 21, 21, 21, 255);
  SDL_RenderClear(renderer);

  {
    sfs::ScopedMemoryTracking tracking{sfs::MemoryTrackingPhase::Render};
    if (registry->hasSystem<RenderSystem>())
    {
      registry->getSystem<RenderSystem>().render(*renderer);
    }
    onRender();
  }

#ifndef NDEBUG
  renderDebugUI(renderer);
#endif

  SDL_RenderPresent(renderer);
}

void Game::destroy()
{
  onDestroy();

  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
}

} // namespace sfs
