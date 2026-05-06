
#include "gameScene.h"
#include "config.h"
#include "engine/systems/collisionSystem.h"
#include "glm/glm/ext/vector_float2.hpp"
#include <engine/components/cameraComponent.h>
#include <engine/components/colliderComponent.h>
#include <engine/components/rigidBodyComponent.h>
#include <engine/components/spriteComponent.h>
#include <engine/components/transformComponent.h>
#include <engine/ecs/entity.h>
#include <engine/input/input.h>
#include <engine/mapLoader/mapLoader.h>
#include <engine/systems/movementSystem.h>
#include <engine/systems/renderSystem.h>
#include <glm/glm/geometric.hpp>
#include <glm/glm/vec2.hpp>

void GameScene::onInit()
{
  loadMap();
  createPlayer();
}

void GameScene::createPlayer()
{
  m_assetStore.addTexture(
      "spritesheet", ASSET_ROOT + "spriteSheets/tilemap.png");
  auto playerSprite = m_assetStore.addSpriteFromSheet(
      "spritesheet", "guy", 16, 16, 16, 6, 1, 0);

  auto enemySprite = m_assetStore.addSpriteFromSheet(
      "spritesheet", "enemy", 16, 16, 0, 6, 1, 0);

  createEntity()
      .addComponent<sfs::TransformComponent>(
          glm::vec2{300, 300}, glm::vec2{2.0, 2.0})
      .addComponent<sfs::ColliderComponent>(
          glm::vec2{0, 0}, glm::vec2{32, 32}, glm::vec2{300, 300})
      .addComponent<sfs::SpriteComponent>(enemySprite)
      .addTag<sfs::Solid>();

  m_player = createEntity()
                 .addComponent<sfs::TransformComponent>(
                     glm::vec2{200.0, 200.0}, glm::vec2{2.0, 2.0})
                 .addComponent<sfs::ColliderComponent>(
                     glm::vec2{0, 0}, glm::vec2{32, 32}, glm::vec2{200, 200})
                 .addComponent<sfs::RigidBodyComponent>(glm::vec2(0.0, 0.0))
                 .addComponent<sfs::SpriteComponent>(playerSprite);

  sfs::Entity camera =
      createEntity()
          .addComponent<sfs::TransformComponent>(
              glm::vec2{0.0f, 0.0f}, glm::vec2{1.0f, 1.0f}, 0.0f)
          .addComponent<sfs::CameraComponent>(
              m_player.getId(), glm::vec2{0.0f, 0.0f}, 8.0f);

  addSystem<sfs::MovementSystem>();
  addSystem<sfs::CollisionSystem>();
  addSystem<sfs::CameraSystem>();
  addSystem<sfs::RenderSystem>(m_assetStore, 800, 600);
}

void GameScene::onProcessInput(const sfs::Input& input)
{
  glm::vec2 direction(0.0f);

  if (input.keyboard().keyHeld(sfs::Key::A))
    direction.x -= 1.0f;
  if (input.keyboard().keyHeld(sfs::Key::D))
    direction.x += 1.0f;
  if (input.keyboard().keyHeld(sfs::Key::W))
    direction.y -= 1.0f;
  if (input.keyboard().keyHeld(sfs::Key::S))
    direction.y += 1.0f;

  if (glm::length(direction) > 0.0f)
  {
    direction = glm::normalize(direction);
  }

  auto& rb = m_player.getComponent<sfs::RigidBodyComponent>();

  rb.velocity = direction * 200.0f;
}

void GameScene::loadMap()
{
  const int tileSize = 32;

  m_assetStore.addTexture("jungle", ASSET_ROOT + "spriteSheets/jungle.png");
  std::vector<uint32_t> jungleSprites = m_assetStore.addSpritesFromSheet(
      "jungle", "jungle", tileSize, tileSize, 0, 0);

  sfs::MapData map =
      sfs::MapLoader::parseMapFile(ASSET_ROOT + "maps/jungle.map");

  for (int y = 0; y < map.height; y++)
  {
    for (int x = 0; x < map.width; x++)
    {
      uint32_t spriteId = map.tiles[y * map.width + x];

      auto tile = createEntity()
                      .addComponent<sfs::TransformComponent>(
                          glm::vec2(x * tileSize, y * tileSize))
                      .addComponent<sfs::SpriteComponent>(spriteId);

      // TODO: Improve this whack ass check
      if (spriteId == 21)
      {
        tile.addComponent<sfs::ColliderComponent>(
            glm::vec2{0, 0},
            glm::vec2{32, 32},
            glm::vec2{x * tileSize, y * tileSize});
        tile.addTag<sfs::Solid>();
      }
    }
  }
}
