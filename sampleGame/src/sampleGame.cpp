
#include "sampleGame.h"
#include "config.h"
#include <SDL_keyboard.h>
#include <engine/mapLoader/mapLoader.h>
#include <engine/systems/movementSystem.h>
#include <engine/systems/renderSystem.h>
#include <glm/glm.hpp>

void SampleGame::onSetup()
{
  loadMap();

  assetStore->addTexture(
      "spritesheet", ASSET_ROOT + "spriteSheets/tilemap.png");

  auto playerSprite = assetStore->addSpriteFromSpriteSheet(
      "spritesheet", "guy", 16, 16, SpriteSheetPosition{16, 6, 1});

  player = registry->createEntity();

  player
      .addComponent<TransformComponent>(
          glm::vec2(windowWidth / 2, windowHeight / 2), glm::vec2(2.0, 2.0))
      .addComponent<RigidBodyComponent>(glm::vec2(0.0, 0.0))
      .addComponent<SpriteComponent>(playerSprite);

  isRunning = true;
}

void SampleGame::onUpdate(double deltaTime)
{
  auto& rb = player.getComponent<RigidBodyComponent>();

  const Uint8* keys = SDL_GetKeyboardState(NULL);

  glm::vec2 direction(0.0f);

  if (keys[SDL_SCANCODE_A])
    direction.x -= 1.0f;
  if (keys[SDL_SCANCODE_D])
    direction.x += 1.0f;
  if (keys[SDL_SCANCODE_W])
    direction.y -= 1.0f;
  if (keys[SDL_SCANCODE_S])
    direction.y += 1.0f;

  if (glm::length(direction) > 0.0f)
  {
    direction = glm::normalize(direction);
  }

  float speed = 100.0f;

  rb.velocity = direction * speed;
}

void SampleGame::loadMap()
{
  const int tileSize = 32;

  assetStore->addTexture("jungle", ASSET_ROOT + "spriteSheets/jungle.png");

  std::vector<uint32_t> jungleSprites = assetStore->addSpritesFromSpriteSheet(
      "jungle", "jungle", tileSize, tileSize, 0);

  MapData map = MapLoader::parseMapFile(ASSET_ROOT + "maps/jungle.map");

  for (int y = 0; y < map.height; y++)
  {
    for (int x = 0; x < map.width; x++)
    {
      uint32_t spriteId = map.tiles[y * map.width + x];

      registry->createEntity()
          .addComponent<TransformComponent>(
              glm::vec2(x * tileSize, y * tileSize))
          .addComponent<SpriteComponent>(spriteId);
    }
  }
}

void SampleGame::onDestroy() {};
