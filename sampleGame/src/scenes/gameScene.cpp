
#include "gameScene.h"
#include "engine/TextRenderer/textRenderer.h"
#include "engine/components/transformComponent.h"
#include "engine/logger/logger.h"
#include "engine/systems/collisionSystem.h"
#include "engine/systems/isometricLightingSystem.h"
#include "engine/systems/isometricRenderSystem.h"
#include "gameObjects/blocks/block.h"
#include "gameObjects/blocks/grass.h"
#include "gameObjects/lamp.h"
#include "gameObjects/player.h"
#include "glm/glm/ext/vector_float3.hpp"
#include <engine/ecs/entity.h>
#include <engine/input/input.h>
#include <engine/mapLoader/mapLoader.h>
#include <engine/systems/movementSystem.h>
#include <engine/systems/renderSystem.h>
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
  auto& lighting = addSystem<sfs::IsometricLightingSystem>();

  renderer.setWorldScale(WORLD_SCALE);

  m_sunController.setLightingSystem(lighting);
}

void GameScene::createEntities()
{
  createObject<Player>();

  createObject<Lamp>(glm::vec2{18.5, 13.5}, glm::vec3{1.0f, 0.05f, 0.05f});
  createObject<Lamp>(glm::vec2{18.5, 17.5}, glm::vec3{0.25f, 0.35f, 1.0f});
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

void GameScene::onPostRender()
{
  auto player = tryFindObject<Player>();

  if (!player)
  {
    LOG_DEBUG("PLAYER NOT FOUND");
    return;
  }

  auto position =
      player->entity().getComponent<sfs::TransformComponent>().position;
  glm::ivec2 playerGrid = glm::ivec2(glm::floor(position));

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
        createObject<GrassBlock>(glm::vec2{x, y}, z, Block::Shape::Half);
      }

      createObject<GrassBlock>(glm::vec2{x, y}, elevation, Block::Shape::Full);
    }
  }
}
