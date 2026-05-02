#include "sampleGame.h"
#include <engine/mapLoader/mapLoader.h>
#include <engine/systems/movementSystem.h>
#include <engine/systems/renderSystem.h>

void SampleGame::onSetup()
{
  assetStore->addTexture(
      "spritesheet", ASSET_ROOT + "spriteSheets/tilemap.png");
  assetStore->addTexture("jungle", ASSET_ROOT + "spriteSheets/jungle.png");

  const int tileSize = 32;
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

  auto playerSprite = assetStore->addSpriteFromSpriteSheet(
      "spritesheet", "guy", 16, 16, SpriteSheetPosition{16, 6, 1});

  registry->createEntity()
      .addComponent<TransformComponent>(
          glm::vec2(10.0, windowHeight / 2), glm::vec2(2.0, 2.0))
      .addComponent<RigidBodyComponent>(glm::vec2(100.0, 0.0))
      .addComponent<SpriteComponent>(playerSprite);

  isRunning = true;
}

void SampleGame::onDestroy() {};
