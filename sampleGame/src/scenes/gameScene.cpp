
#include "gameScene.h"
#include "engine/TextRenderer/textRenderer.h"
#include "engine/components/transformComponent.h"
#include "engine/logger/logger.h"
#include "engine/systems/cameraSystem.h"
#include "engine/systems/collisionSystem.h"
#include "engine/systems/isometric/isometricRenderSystem.h"
#include "engine/systems/isometric/isometricShadowSystem.h"
#include "engine/systems/isometric/isometricSpriteShadowSystem.h"
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

#include <string>
#include <tracy/public/tracy/Tracy.hpp>

void GameScene::onInit()
{
  createEntities();

  addSystem<sfs::MovementSystem>();
  addSystem<sfs::CollisionSystem>();
  addSystem<sfs::CameraSystem>();
  addSystem<TerrainGeneratorSystem>(*this);
  auto& renderer = addSystem<sfs::IsometricRenderSystem>(
      m_assetStore, WINDOW_WIDTH, WINDOW_HEIGHT, 32, 16);

  sfs::IsometricShadowSettings shadowSettings = {7.0f, 7.0f};

  addSystem<sfs::IsometricShadowSystem>(shadowSettings, &m_assetStore);
  addSystem<sfs::IsometricSpriteShadowSystem>(shadowSettings, m_assetStore);

  addSystem<SunController>();

  renderer.setWorldScale(WORLD_SCALE);
}

void GameScene::createEntities()
{
  createObject<Player>();

  createObject<Lamp>(glm::vec2{18.5, 13.5}, Lamp::Color::Blue);
  createObject<Lamp>(glm::vec2{18.5, 17.5}, Lamp::Color::SoftRed);
  createObject<Lamp>(glm::vec2{5.5, 13.5});
  createObject<Lamp>(glm::vec2{6.5, 13.5});
}

void GameScene::onProcessInput(const sfs::Input& input)
{
  if (input.keyboard().keyPressed(sfs::Key::F))
    m_sunController.toggleSun();
}

void GameScene::onRender()
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
  sfs::TextRenderer::drawText(
      20, 40, "FPS: " + std::to_string(static_cast<int>(m_fps)));
  sfs::TextRenderer::drawText(
      20, 60, "Time: " + getSystem<SunController>().timeString12Hour());
}

void GameScene::onUpdate(double deltaTime)
{

  if (deltaTime > 0.0001)
  {
    const double instantFps = 1.0 / deltaTime;

    m_fps += (instantFps - m_fps) * 0.1;
  }

  FrameMark;
}
