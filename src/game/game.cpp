
#include "game.h"
#include "SDL.h"
#include "SDL_render.h"
#include "components/rigidBodyComponent.h"
#include "components/spriteComponent.h"
#include "components/transformComponent.h"
#include "glm/ext/vector_float2.hpp"
#include "logger/logger.h"
#include "systems/movementSystem.h"
#include "systems/renderSystem.h"

#include <glm/ext/vector_float3.hpp>
#include <memory>

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

void Game::update(double deltaTime) { registry->update(deltaTime); }

void Game::render()
{

  // Render background
  SDL_SetRenderDrawColor(renderer, 21, 21, 21, 255);
  SDL_RenderClear(renderer);

  if (registry->hasSystem<RenderSystem>())
  {
    registry->getSystem<RenderSystem>().render(*renderer);
  }
}

void Game::destroy()
{
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
}

void Game::setup()
{
  registry = std::make_unique<Registry>();
  assetStore = std::make_unique<AssetStore>(*renderer);

  registry->addSystem<MovementSystem>();
  registry->addSystem<RenderSystem>(*assetStore);

  assetStore->addTexture("player", "./assets/sprites/man_guy.png");
  assetStore->addTexture("truck", "./assets/spriteSheets/tilemap.png");

  // TODO: I HATE that clangd intellisense doesn't infer TAargs.
  // Possibly make addComponent take in place construction
  // if I can live with myself adding a an extra move for cosmetics...
  registry->createEntity()
      .addComponent<TransformComponent>(
          glm::vec2(10.0, windowHeight / 2), glm::vec2(6.0, 6.0))
      .addComponent<RigidBodyComponent>(glm::vec2(100.0, 0.0))
      .addComponent<SpriteComponent>(
          "truck", 16, 16, glm::vec3(16.0, 6.0, 1.0));

  isRunning = true;
}
