
#include "gameScene.h"
#include "controllers/sunController.h"
#include "effects/particleEffects.h"
#include "engine/core/components/lightEmitterComponent.h"
#include "engine/core/components/particleEmitterComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/logger/logger.h"
#include "engine/core/scripting/luaScripting.h"
#include "engine/runtime/TextRenderer/textRenderer.h"
#include "engine/runtime/particles/prefabs.h"
#include "engine/runtime/rendering/modules/isometricWater.h"
#include "engine/runtime/rendering/modules/particles.h"
#include "engine/runtime/rendering/modules/spriteShadow.h"
#include "engine/runtime/rendering/modules/terrainShadow.h"
#include "engine/runtime/rendering/modules/voxelTerrain.h"
#include "engine/runtime/rendering/util/isometric/isometricLightingUtils.h"
#include "engine/runtime/systems/cameraSystem.h"
#include "engine/runtime/systems/collisionSystem.h"
#include "engine/runtime/systems/isometric/isometricRenderSystem.h"
#include "engine/runtime/voxel/voxelWorld.h"
#include "gameObjects/lamp.h"
#include "gameObjects/player.h"
#include <engine/core/ecs/entity.h>
#include <engine/core/mapLoader/mapLoader.h>
#include <engine/runtime/input/input.h>
#include <engine/runtime/systems/movementSystem.h>
#include <glm/glm/geometric.hpp>
#include <glm/glm/vec2.hpp>

#include <engine/core/util/profiling.h>
#include <string>

#ifndef ENGINE_WEB
  #include <imgui.h>
#endif

using IsometricParticles = sfs::Particles<sfs::IsometricRenderContext>;

void GameScene::onInit()
{
  createEntities();

  addSystem<sfs::MovementSystem>();
  addSystem<sfs::CollisionSystem>();
  addSystem<sfs::CameraSystem>();

  // Voxel terrain: the engine owns storage/streaming/meshing; the game supplies
  // the block types + generator. World height 0..16 blocks; streamed wide
  // enough (a square) to cover the screen's diamond corners + the shadow
  // window. The VoxelTerrain module culls drawing to the on-screen diamond, so
  // the wide radius costs storage/meshing (cached) but not per-frame draw.
  m_blockRegistry.setup(m_assetStore);
  auto& voxelWorld = addSystem<sfs::VoxelWorld>(0, 16, 38);
  voxelWorld.setGenerator(&m_voxelGenerator);
  voxelWorld.setBlockRegistry(&m_blockRegistry);

  auto& renderer =
      addSystem<sfs::IsometricRenderSystem>(m_assetStore, quadRenderer());

  renderer.withModules<sfs::TerrainShadow,
                       sfs::SpriteShadow,
                       sfs::IsometricWater>();

  constexpr float shadowMaxLength = 3.5f;
  renderer.module<sfs::TerrainShadow>()
      ->shadowSettings()
      .terrainShadowMaxLength = shadowMaxLength;
  renderer.module<sfs::SpriteShadow>()->shadowSettings().spriteShadowMaxLength =
      shadowMaxLength;

  renderer.withModule<sfs::VoxelTerrain>().setWorld(
      voxelWorld, m_blockRegistry);

  renderer.module<sfs::IsometricWater>()->setWaterSurfaceSource(&voxelWorld);

  auto& particles = renderer.withModule<IsometricParticles>();
  sfs::registerBloodEffects(particles);
  sfs::registerBloodEffects(particles,
                            "ichor",
                            glm::vec3{0.15f, 0.55f, 1.0f},
                            glm::vec3{0.0f, 0.08f, 0.35f});
  particles.registerEffect("embers", sfs::emberEffect());
  particles.enableStains(&voxelWorld);

  auto& sun = addSystem<SunController>();

  if (sfs::LuaScripting* lua = sfs::activeLua())
    lua->registerConfig(sun);

  // The voxel world's surface (topmost solid) feeds the point-light occlusion
  // heightmap, decals, and picking -- hole-free as chunks stream in.
  getSystem<sfs::IsometricRenderSystem>().setTerrainHeightSource(&voxelWorld);

  // Same surface drives movement so actors are blocked by cliffs (step up to
  // one level, slide along anything taller).
  getSystem<sfs::MovementSystem>().setTerrainHeightSource(&voxelWorld);
}

void GameScene::createEntities()
{
  m_player = &createObject<Player>();

  auto& emberLamp =
      createObject<Lamp>(glm::vec2{16.5, 16.5}, Lamp::Color::Pink);
  emberLamp.entity().addComponent<sfs::ParticleEmitterComponent>(
      "embers", glm::vec2{0.0f, 0.0f}, 0.4f);

  createObject<Lamp>(glm::vec2{16.5, 11.5}, Lamp::Color::Moonlight);
  createObject<Lamp>(glm::vec2{5.5, 13.5});
  createObject<Lamp>(glm::vec2{6.5, 13.5});
}

void GameScene::onProcessInput(const sfs::Input& input)
{
  auto& render = getSystem<sfs::IsometricRenderSystem>();

  m_mousePos = input.mouse().getPosition();

  const sfs::TilePick pick = render.pickTile(m_mousePos);

  m_hoveredTile = pick.tile;
  m_hoveredElevation = pick.elevation;
  m_hasHoveredTile = pick.valid;

  if (m_hasHoveredTile && m_player &&
      input.mouse().mouseHeld(sfs::MouseButton::Right))
  {
    const glm::vec2 from =
        m_player->entity().getComponent<sfs::TransformComponent>().position;

    glm::vec2 dir = pick.world - from;
    const float len = glm::length(dir);
    dir = len > 0.0001f ? dir / len : glm::vec2{1.0f, 0.0f};

    // Layered shotgun gore blast along the shot direction.
    spawnGore(*render.module<IsometricParticles>(),
              pick.world,
              static_cast<float>(pick.elevation),
              dir,
              12.0f,
              "blood");
  }

  if (m_hasHoveredTile && input.mouse().mouseHeld(sfs::MouseButton::Left))
  {
    const int z = (m_hoveredElevation - 1) / sfs::kLevelsPerBlock;
    getSystem<sfs::VoxelWorld>().setBlock(
        m_hoveredTile.x, m_hoveredTile.y, z, sfs::kAirBlock);
  }
}

void GameScene::onRender()
{
  auto player = tryFindObject<Player>();

  if (!player)
  {
    LOG_DEBUG("PLAYER NOT FOUND");
    return;
  }

  textRenderer().drawText(
      20, 20, "FPS: " + std::to_string(static_cast<int>(m_fps)));
  textRenderer().drawText(
      20, 40, "Time: " + getSystem<SunController>().timeString12Hour());

  if (m_hasHoveredTile)
  {
    getSystem<sfs::IsometricRenderSystem>().drawDebugTile(
        glm::vec2{m_hoveredTile},
        m_hoveredElevation,
        SDL_Color{255, 255, 0, 255});
  }

  if (getSystem<sfs::CollisionSystem>().debugDraw())
    getSystem<sfs::IsometricRenderSystem>().drawDebugColliders();
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

  if (m_player && m_player->entity().hasComponent<sfs::LightEmitterComponent>())
  {
    auto& light = m_player->entity().getComponent<sfs::LightEmitterComponent>();

    ImGui::SetNextWindowSize(ImVec2(300, 180), ImGuiCond_FirstUseEver);
    ImGui::Begin("Player Light");

    // radius and height are in screen pixels (see LightEmitterComponent).
    ImGui::SliderFloat("Radius (px)", &light.radius, 0.0f, 2000.0f);
    ImGui::SliderFloat("Strength", &light.intensity, 0.0f, 4.0f);
    ImGui::SliderFloat("Height (px)", &light.height, 0.0f, 256.0f);
    ImGui::ColorEdit3("Color", &light.color.x);

    ImGui::End();
  }
#endif
}
