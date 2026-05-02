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
  setMemoryTrackingEnabled(true);

  registry = std::make_unique<Registry>();
  assetStore = std::make_unique<AssetStore>(*renderer);

  registry->addSystem<MovementSystem>();
  registry->addSystem<RenderSystem>(*assetStore);

  onSetup();

  setMemoryTrackingEnabled(false);
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
  SDL_Event sdlEvent;

  while (SDL_PollEvent(&sdlEvent))
  {
    ImGui_ImplSDL2_ProcessEvent(&sdlEvent);

    switch (sdlEvent.type)
    {
    case SDL_QUIT:
      isRunning = false;
      break;

    case SDL_KEYDOWN:
      if (sdlEvent.key.keysym.sym == SDLK_ESCAPE)
      {
        isRunning = false;
      }
      break;
    }

    onProcessInput(sdlEvent);
  }
}

void Game::update(double deltaTime)
{
  setMemoryTrackingEnabled(true);
  registry->update(deltaTime);
  onUpdate(deltaTime);
  setMemoryTrackingEnabled(false);
}

void Game::render()
{
  // Render background
  SDL_SetRenderDrawColor(renderer, 21, 21, 21, 255);
  SDL_RenderClear(renderer);

  setMemoryTrackingEnabled(true);
  if (registry->hasSystem<RenderSystem>())
  {
    registry->getSystem<RenderSystem>().render(*renderer);
  }
  onRender();
  setMemoryTrackingEnabled(false);

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
