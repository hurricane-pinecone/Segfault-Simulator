
#include "sampleGame.h"
#include "config.h"
#include <SDL_keyboard.h>
#include <engine/mapLoader/mapLoader.h>
#include <engine/systems/movementSystem.h>
#include <engine/systems/renderSystem.h>
#include <glm/glm.hpp>

#include "systems/playerInputSystem.h"

void SampleGame::onSetup()
{
  loadMap();

  assetStore->addTexture(
      "spritesheet", ASSET_ROOT + "spriteSheets/tilemap.png");

  auto playerSprite = assetStore->addSpriteFromSpriteSheet(
      "spritesheet", "guy", 16, 16, sfs::SpriteSheetPosition{16, 6, 1});

  player = registry->createEntity();

  player.addComponent<PlayerTag>()
      .addComponent<sfs::TransformComponent>(
          glm::vec2(windowWidth / 2, windowHeight / 2), glm::vec2(2.0, 2.0))
      .addComponent<sfs::RigidBodyComponent>(glm::vec2(0.0, 0.0))
      .addComponent<sfs::SpriteComponent>(playerSprite);

  // Register Game defined systems
  registry->addSystem<PlayerInputSystem>();

  isRunning = true;
}

void SampleGame::loadMap()
{
  const int tileSize = 32;

  assetStore->addTexture("jungle", ASSET_ROOT + "spriteSheets/jungle.png");

  std::vector<uint32_t> jungleSprites = assetStore->addSpritesFromSpriteSheet(
      "jungle", "jungle", tileSize, tileSize, 0);

  sfs::MapData map =
      sfs::MapLoader::parseMapFile(ASSET_ROOT + "maps/jungle.map");

  for (int y = 0; y < map.height; y++)
  {
    for (int x = 0; x < map.width; x++)
    {
      uint32_t spriteId = map.tiles[y * map.width + x];

      registry->createEntity()
          .addComponent<sfs::TransformComponent>(
              glm::vec2(x * tileSize, y * tileSize))
          .addComponent<sfs::SpriteComponent>(spriteId);
    }
  }
}

void SampleGame::onDestroy() {};
