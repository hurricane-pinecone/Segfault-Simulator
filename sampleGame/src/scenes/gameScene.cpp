
#include "gameScene.h"
#include "config.h"
#include "engine/TextRenderer/textRenderer.h"
#include "engine/systems/collisionSystem.h"
#include "engine/systems/isometricRenderSystem.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include <SDL_rect.h>
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
#include <string>

void GameScene::onInit()
{
  loadMap();
  createEntities();

  addSystem<sfs::MovementSystem>();
  addSystem<sfs::CollisionSystem>();
  addSystem<sfs::CameraSystem>();
  // addSystem<sfs::RenderSystem>(m_assetStore, 800, 600);
  addSystem<sfs::IsometricRenderSystem>(m_assetStore, 800, 600, 32, 16);
}

void GameScene::createEntities()
{
  m_assetStore.addTexture(
      "spritesheet", ASSET_ROOT + "spriteSheets/tilemap.png");
  auto playerSprite = m_assetStore.addSpriteFromSheet(
      "spritesheet", "guy", 16, 16, 16, 6, 1, 0);

  auto enemySprite = m_assetStore.addSpriteFromSheet(
      "spritesheet", "enemy", 16, 16, 0, 6, 1, 0);

  m_player = createEntity()
                 .addComponent<sfs::SpriteComponent>(
                     playerSprite, glm::vec2{0.5f, 1.0f})
                 .addComponent<sfs::TransformComponent>(
                     glm::vec2{12.0, 12.0}, glm::vec2{1.0, 1.0})
                 .addComponent<sfs::ColliderComponent>(
                     glm::vec2{0, 0}, glm::vec2{32, 32}, glm::vec2{200, 200})
                 .addComponent<sfs::RigidBodyComponent>(glm::vec2{0.0, 0.0});

  createEntity()
      .addComponent<sfs::TransformComponent>(
          glm::vec2{0.0f, 0.0f}, glm::vec2{1.0f, 1.0f}, 0.0f)
      .addComponent<sfs::CameraComponent>(
          m_player.getId(), glm::vec2{0.0f, 0.0f}, 8.0f);
}

void GameScene::onProcessInput(const sfs::Input& input)
{
  int mouseX = 0;
  int mouseY = 0;
  SDL_GetMouseState(&mouseX, &mouseY);

  getSystem<sfs::IsometricRenderSystem>().setLightPosition(mouseX, mouseY, 128);

  glm::vec2 screenDirection(0.0f);

  if (input.keyboard().keyHeld(sfs::Key::A))
    screenDirection.x -= 1.0f;
  if (input.keyboard().keyHeld(sfs::Key::D))
    screenDirection.x += 1.0f;
  if (input.keyboard().keyHeld(sfs::Key::W))
    screenDirection.y -= 1.0f;
  if (input.keyboard().keyHeld(sfs::Key::S))
    screenDirection.y += 1.0f;

  if (glm::length(screenDirection) > 0.0f)
  {
    screenDirection = glm::normalize(screenDirection);
  }

  glm::vec2 gridDirection{screenDirection.y + screenDirection.x,
                          screenDirection.y - screenDirection.x};

  if (glm::length(gridDirection) > 0.0f)
  {
    gridDirection = glm::normalize(gridDirection);
  }

  auto& rb = m_player.getComponent<sfs::RigidBodyComponent>();
  rb.velocity = gridDirection * 5.0f;
}

void GameScene::onRender(SDL_Renderer& renderer)
{
  const auto& playerTransform =
      m_player.getComponent<sfs::TransformComponent>();

  glm::vec2 playerTile = glm::floor(playerTransform.position);

  int elevation = 0;

  if (playerTile.y < 10)
  {
    elevation = 10 - static_cast<int>(playerTile.y);
  }

  getSystem<sfs::IsometricRenderSystem>().drawDebugTile(
      renderer, playerTile, elevation);
}

void GameScene::onPostRender()
{
  auto& pos = m_player.getComponent<sfs::TransformComponent>().position;
  glm::ivec2 playerGrid = glm::ivec2(glm::floor(pos));

  sfs::TextRenderer::drawText(20,
                              20,
                              "Pos: " + std::to_string(playerGrid.x) + ", " +
                                  std::to_string(playerGrid.y));
}

void GameScene::onUpdate(double deltaTime)
{
  m_worldWaveTime += static_cast<float>(deltaTime);

  getSystem<sfs::IsometricRenderSystem>().setWaveTime(m_worldWaveTime);
}

void GameScene::loadMap()
{
  const int tileSize = 32;

  m_assetStore.addTexture("block", ASSET_ROOT + "sprites/block.png");
  m_assetStore.addTexture(
      "blockNormal", ASSET_ROOT + "sprites/block_normal.png");
  m_assetStore.addTexture("blockHalf", ASSET_ROOT + "sprites/block_half.png");
  auto blockSprite =
      m_assetStore.addSprite("block", "block", SDL_Rect{0, 0, 32, 32});
  auto blockNormal = m_assetStore.addSprite(
      "blockNormal", "blockNormal", SDL_Rect{0, 0, 32, 32});

  auto blockHalf =
      m_assetStore.addSprite("blockHalf", "blockHalf", SDL_Rect{0, 0, 32, 32});

  // sfs::MapData map =
  //     sfs::MapLoader::parseMapFile(ASSET_ROOT + "maps/jungle.map");

  for (int y = 0; y < 25; y++)
  {
    for (int x = 0; x < 25; x++)
    {
      int elevation = 0;

      // Hill at the top of the map
      if (y < 10)
      {
        elevation = 10 - y;
      }

      for (int z = 0; z < elevation; z++)
      {
        createEntity()
            .addComponent<sfs::TransformComponent>(glm::vec2{x, y})
            .addComponent<sfs::SpriteComponent>(blockHalf)
            .addComponent<sfs::ElevationComponent>(z)
            .addTag<sfs::IsometricTile>();
      }

      createEntity()
          .addComponent<sfs::TransformComponent>(glm::vec2{x, y})
          .addComponent<sfs::SpriteComponent>(blockSprite)
          .addComponent<sfs::NormalMapComponent>(blockNormal)
          .addComponent<sfs::ElevationComponent>(elevation)
          .addTag<sfs::IsometricTile>();
    }
  }
}
