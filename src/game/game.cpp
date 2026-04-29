
#include "game.h"
#include "SDL.h"
#include "components/rigidBodyComponent.h"
#include "components/spriteComponent.h"
#include "components/transformComponent.h"
#include "glm/ext/vector_float2.hpp"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "logger/logger.h"
#include "systems/movementSystem.h"
#include "systems/renderSystem.h"
#include "utils/allocationMetrics.h"
#include "utils/ui.h"

#include <glm/ext/vector_float3.hpp>
#include <memory>
#include <string>

Game::Game() { isRunning = false; }

Game::~Game() { LOG_INFO("Game destroyed"); }

void Game::init()
{
  Logger::setVerbosity(Logger::Verbosity::FULL);
  Logger::setLogLevel(Logger::Level::DEBUG);

  if (SDL_Init(SDL_INIT_EVERYTHING) != 0)
  {
    LOG_ERROR("Error initializing SDL");
    return;
  }

  SDL_DisplayMode displayMode;
  SDL_GetCurrentDisplayMode(0, &displayMode);

  windowWidth = 800;
  windowHeight = 600;

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

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);

  // SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
}

void Game::run()
{
  setup();

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
  }
}

void Game::update(double deltaTime)
{
  setMemoryTrackingEnabled(true);
  registry->update(deltaTime);
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
  setMemoryTrackingEnabled(false);

#ifndef NDEBUG
  renderDebugUI(renderer);
#endif

  SDL_RenderPresent(renderer);
}

void Game::destroy()
{
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
}

void Game::setup()
{
  setMemoryTrackingEnabled(true);

  registry = std::make_unique<Registry>();
  assetStore = std::make_unique<AssetStore>(*renderer);

  registry->addSystem<MovementSystem>();
  registry->addSystem<RenderSystem>(*assetStore);

  assetStore->addTexture("truck", "./assets/spriteSheets/tilemap.png");
  assetStore->addTexture("jungle", "./assets/spriteSheets/jungle.png");

  // TODO: I HATE that clangd intellisense doesn't infer TAargs.
  // Possibly make addComponent take in place construction
  // if I can live with myself adding an extra move...
  registry->createEntity()
      .addComponent<TransformComponent>(
          glm::vec2(10.0, windowHeight / 2), glm::vec2(6.0, 6.0))
      .addComponent<RigidBodyComponent>(glm::vec2(100.0, 0.0))
      .addComponent<SpriteComponent>(
          "truck", 16, 16, glm::vec3(16.0, 6.0, 1.0));

  isRunning = true;

  setMemoryTrackingEnabled(false);
}
