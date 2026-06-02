
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

#include <engine/utils/profiling.h>
#include <string>

#ifndef ENGINE_WEB
  #include <imgui.h>
#endif

void GameScene::onInit()
{
  createEntities();

  addSystem<sfs::MovementSystem>();
  addSystem<sfs::CollisionSystem>();
  addSystem<sfs::CameraSystem>();
  addSystem<TerrainGeneratorSystem>(*this);

  addSystem<sfs::IsometricRenderSystem>(m_assetStore, quadRenderer());

  // Terrain and sprite shadows share one length so equal heights match.
  constexpr float shadowMaxLength = 3.5f;
  sfs::IsometricShadowSettings shadowSettings = {shadowMaxLength, shadowMaxLength};

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

  textRenderer().drawText(20, 20, stream.str());
  textRenderer().drawText(
      20, 40, "FPS: " + std::to_string(static_cast<int>(m_fps)));
  textRenderer().drawText(
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

void GameScene::onDebugUI()
{
#ifndef ENGINE_WEB
  if (!hasSystem<SunController>())
    return;

  auto& sun = getSystem<SunController>();

  ImGui::SetNextWindowSize(ImVec2(300, 150), ImGuiCond_FirstUseEver);
  ImGui::Begin("Time of Day");

  ImGui::Text("%s", sun.timeString12Hour().c_str());

  bool enabled = sun.sunEnabled();
  if (ImGui::Checkbox("Sun enabled", &enabled))
    sun.setSunEnabled(enabled);

  float hour = sun.timeOfDay() * 24.0f;
  if (ImGui::SliderFloat("Hour", &hour, 0.0f, 24.0f))
    sun.setTimeOfDay(hour / 24.0f);

  float multiplier = sun.timeMultiplier();
  if (ImGui::SliderFloat("Day speed", &multiplier, 0.0f, 10.0f))
    sun.setTimeMultiplier(multiplier);

  ImGui::End();
#endif
}
