
#include "gameScene.h"
#include "engine/TextRenderer/textRenderer.h"
#include "engine/components/transformComponent.h"
#include "engine/logger/logger.h"
#include "engine/systems/cameraSystem.h"
#include "engine/systems/collisionSystem.h"
#include "engine/systems/isometric/isometricRenderSystem.h"
#include "engine/systems/isometric/isometricShadowSystem.h"
#include "engine/utils/isometricLightingUtils.h"
#include "gameObjects/lamp.h"
#include "gameObjects/player.h"
#include "systems/TerrainGeneratorSystem.h"
#include <engine/ecs/entity.h>
#include <engine/input/input.h>
#include <engine/mapLoader/mapLoader.h>
#include <engine/systems/movementSystem.h>
#include <engine/systems/renderSystem.h>
#include <glm/glm/vec2.hpp>
#include <iomanip>
#include <sstream>

void GameScene::onInit()
{
  createEntities();

  addSystem<sfs::MovementSystem>();
  addSystem<sfs::CollisionSystem>();
  addSystem<sfs::CameraSystem>();
  addSystem<TerrainGeneratorSystem>(*this);
  auto& renderer = addSystem<sfs::IsometricRenderSystem>(
      m_assetStore, WINDOW_WIDTH, WINDOW_HEIGHT, 32, 16);
  auto& lighting = addSystem<sfs::IsometricLightingSystem>();
  addSystem<sfs::IsometricShadowSystem>(
      sfs::IsometricShadowSettings{5.0f, 7.0f});

  renderer.setWorldScale(WORLD_SCALE);

  m_sunController.setLightingSystem(lighting);
}

void GameScene::createEntities()
{
  createObject<Player>();

  createObject<Lamp>(glm::vec2{18.5, 13.5}, Lamp::Color::Red);
  createObject<Lamp>(glm::vec2{18.5, 17.5}, Lamp::Color::Blue);
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

  const auto position =
      player->entity().getComponent<sfs::TransformComponent>().position;

  std::ostringstream stream;

  stream << std::fixed << std::setprecision(2) << "Pos: " << position.x << ", "
         << position.y;

  sfs::TextRenderer::drawText(20, 20, stream.str());
}

void GameScene::onUpdate(double deltaTime)
{
  m_worldWaveTime += static_cast<float>(deltaTime);

  getSystem<sfs::IsometricRenderSystem>().setWaveTime(m_worldWaveTime);
}

// void GameScene::loadMap()
// {
//   const int tileSize = 32;
//
//   for (int y = 0; y < 25; y++)
//   {
//     for (int x = 0; x < 25; x++)
//     {
//       int elevation = 0;
//
//       // Hill at the top of the map
//       if (y < 10)
//       {
//         elevation = 10 - y;
//       }
//
//       for (int z = 0; z < elevation; z++)
//       {
//         createObject<GrassBlock>(glm::vec2{x, y}, z, Block::Shape::Half);
//       }
//
//       createObject<GrassBlock>(glm::vec2{x, y}, elevation,
//       Block::Shape::Full);
//     }
//   }
// }
