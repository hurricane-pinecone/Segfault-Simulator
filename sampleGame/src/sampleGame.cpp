
#include "sampleGame.h"
#include "config.h"
#include <SDL_keyboard.h>
#include <engine/components/cameraComponent.h>
#include <engine/mapLoader/mapLoader.h>
#include <engine/systems/cameraSystem.h>
#include <engine/systems/movementSystem.h>
#include <engine/systems/renderSystem.h>
#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_int2.hpp>
#include <glm/glm.hpp>

void SampleGame::onSetup()
{
  loadMap();

  assetStore->addTexture(
      "spritesheet", ASSET_ROOT + "spriteSheets/tilemap.png");

  assetStore->addTexture("plants", ASSET_ROOT + "spriteSheets/vines.png");
  assetStore->loadAsepriteAtlas(
      "plants", ASSET_ROOT + "spriteSheets/vines.json");

  auto playerSprite =
      assetStore->addSpriteFromSheet("spritesheet", "guy", 16, 16, 16, 6, 1, 0);

  player = registry->createEntity()
               .addComponent<sfs::TransformComponent>(
                   glm::ivec2{windowWidth / 2, windowHeight / 2},
                   glm::vec2{2.0, 2.0})
               .addComponent<sfs::RigidBodyComponent>(glm::vec2(0.0, 0.0))
               .addComponent<sfs::SpriteComponent>(playerSprite);

  auto camera = registry->createEntity()
                    .addComponent<sfs::TransformComponent>(
                        glm::vec2{0.0f, 0.0f}, glm::vec2{1.0f, 1.0f}, 0.0f)
                    .addComponent<sfs::CameraComponent>(
                        player.getId(), glm::vec2{0.0f, 0.0f}, 8.0f);

  registry->addSystem<sfs::CameraSystem>();

  isRunning = true;
}

void SampleGame::onProcessInput(SDL_Event& event)
{
  // GetKeyboardState is better for button held checks. IE, movement checks.
  // For single presses, use SDL_Event.
  const Uint8* keys = SDL_GetKeyboardState(nullptr);

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

  auto& rb = player.getComponent<sfs::RigidBodyComponent>();

  rb.velocity = direction * 200.0f;
}

void SampleGame::loadMap()
{
  const int tileSize = 32;

  assetStore->addTexture("jungle", ASSET_ROOT + "spriteSheets/jungle.png");

  std::vector<uint32_t> jungleSprites = assetStore->addSpritesFromSheet(
      "jungle", "jungle", tileSize, tileSize, 0, 0);

  sfs::MapData map =
      sfs::MapLoader::parseMapFile(ASSET_ROOT + "maps/jungle.map");

  for (int y = 0; y < map.height; y++)
  {
    for (int x = 0; x < map.width; x++)
    {
      uint32_t spriteId = map.tiles[y * map.width + x];

      auto tile = registry->createEntity()
                      .addComponent<sfs::TransformComponent>(
                          glm::vec2(x * tileSize, y * tileSize))
                      .addComponent<sfs::SpriteComponent>(spriteId);
    }
  }
}

void SampleGame::onDestroy() {};
