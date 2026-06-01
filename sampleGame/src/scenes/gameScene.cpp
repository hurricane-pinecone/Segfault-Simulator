
#include "gameScene.h"
#include "engine/TextRenderer/textRenderer.h"
#include "engine/components/transformComponent.h"
#include "engine/logger/logger.h"
#include "engine/systems/cameraSystem.h"
#include "engine/systems/collisionSystem.h"
#include "engine/systems/isometric/isometricRenderSystem.h"
#include "engine/systems/isometric/isometricShadowSystem.h"
#include "engine/systems/isometric/isometricSpriteShadowSystem.h"
#include "engine/systems/isometric/isometricWaterSystem.h"
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

  // The isometric projection is owned by the game and injected into this
  // system each frame (see SampleGame); the scene just registers the system.
  addSystem<sfs::IsometricRenderSystem>(m_assetStore);

  sfs::IsometricShadowSettings shadowSettings = {7.0f, 7.0f};

  addSystem<sfs::IsometricShadowSystem>(shadowSettings, &m_assetStore);
  addSystem<sfs::IsometricSpriteShadowSystem>(shadowSettings, m_assetStore);
  addSystem<sfs::IsometricWaterSystem>();

  addSystem<SunController>();
}

void GameScene::createEntities()
{
  createObject<Player>();

  createObject<Lamp>(glm::vec2{16.5, 16.5}, Lamp::Color::Pink);
  createObject<Lamp>(glm::vec2{16.5, 11.5}, Lamp::Color::Moonlight);
  createObject<Lamp>(glm::vec2{5.5, 13.5});
  createObject<Lamp>(glm::vec2{6.5, 13.5});
}

void GameScene::onProcessInput(const sfs::Input& input)
{
  if (input.keyboard().keyPressed(sfs::Key::F))
    m_sunController.toggleSun();

  // Resolve the tile under the cursor so onRender can highlight it. The pick
  // accounts for tile elevation, so raised terrain highlights correctly.
  m_mousePos = input.mouse().getPosition();

  const sfs::TilePick pick =
      getSystem<sfs::IsometricRenderSystem>().pickTile(m_mousePos);

  m_hoveredTile = pick.tile;
  m_hoveredElevation = pick.elevation;
  m_hasHoveredTile = pick.valid;
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

  if (m_hasHoveredTile)
  {
    getSystem<sfs::IsometricRenderSystem>().drawDebugTile(
        glm::vec2{m_hoveredTile},
        m_hoveredElevation,
        SDL_Color{255, 255, 0, 255});
  }
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
