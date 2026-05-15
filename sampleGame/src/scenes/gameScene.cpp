
#include "gameScene.h"
#include "config.h"
#include "engine/systems/collisionSystem.h"
#include "engine/systems/isometricLightingSystem.h"
#include "engine/systems/isometricRenderSystem.h"
#include "gameObjects/blocks/block.h"
#include "gameObjects/blocks/grass.h"
#include "gameObjects/lamp.h"
#include "gameObjects/player.h"
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

void GameScene::onInit()
{
  loadMap();
  createEntities();

  addSystem<sfs::MovementSystem>();
  addSystem<sfs::CollisionSystem>();
  addSystem<sfs::CameraSystem>();
  auto& renderer = addSystem<sfs::IsometricRenderSystem>(
      m_assetStore, WINDOW_WIDTH, WINDOW_HEIGHT, 32, 16);
  addSystem<sfs::IsometricLightingSystem>();

  renderer.setWorldScale(WORLD_SCALE);

  m_sunController.setRenderSystem(renderer);
}

void GameScene::createEntities()
{
  createObject<Player>();

  createObject<Lamp>(glm::vec2{18.5, 13.5});
  createObject<Lamp>(glm::vec2{18.5, 17.5});
  createObject<Lamp>(glm::vec2{5.5, 13.5});
  createObject<Lamp>(glm::vec2{6.5, 13.5});
}

void GameScene::onProcessInput(const sfs::Input& input)
{
  auto mousePos = input.mouse().getPosition();
  m_sunController.moveTo(mousePos.x, mousePos.y);

  if (input.keyboard().keyPressed(sfs::Key::F))
    m_sunController.toggleSun();
}

void GameScene::onRender()
{

  // const auto& playerTransform =
  //     m_player.getComponent<sfs::TransformComponent>();
  //
  // glm::vec2 playerTile = glm::floor(playerTransform.position);
  //
  // int elevation = 0;
  //
  // if (playerTile.y < 10)
  // {
  //   elevation = 10 - static_cast<int>(playerTile.y);
  // }

  // getSystem<sfs::IsometricRenderSystem>().drawDebugTile(
  //     renderer, playerTile, elevation);
}

void GameScene::onPostRender()
{
  // auto& pos = m_player.getComponent<sfs::TransformComponent>().position;
  // glm::ivec2 playerGrid = glm::ivec2(glm::floor(pos));

  // sfs::TextRenderer::drawText(20,
  //                             20,
  //                             "Pos: " + std::to_string(playerGrid.x) + ", " +
  //                                 std::to_string(playerGrid.y));
}

void GameScene::onUpdate(double deltaTime)
{
  m_worldWaveTime += static_cast<float>(deltaTime);

  getSystem<sfs::IsometricRenderSystem>().setWaveTime(m_worldWaveTime);
}

void GameScene::loadMap()
{
  const int tileSize = 32;

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
        createObject<GrassBlock>(glm::vec2{x, y}, z, BlockShape::Half);
      }

      createObject<GrassBlock>(glm::vec2{x, y}, elevation);
    }
  }
}
