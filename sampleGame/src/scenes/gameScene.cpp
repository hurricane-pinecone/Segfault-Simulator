
#include "gameScene.h"
#include "controllers/sunController.h"
#include "effects/particleEffects.h"
#include "engine/core/components/particleEmitterComponent.h"
#include "engine/core/components/pointLightComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/logger/logger.h"
#include "engine/core/scripting/luaScripting.h"
#include "engine/runtime/particles/prefabs.h"
#include "engine/runtime/rendering3d/modules/decalModule.h"
#include "engine/runtime/rendering3d/modules/particleModule.h"
#include "engine/runtime/rendering3d/modules/spriteModule.h"
#include "engine/runtime/rendering3d/modules/uiModule.h"
#include "engine/runtime/rendering3d/modules/voxelModule.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/runtime/systems/cameraSystem.h"
#include "engine/runtime/systems/collisionSystem.h"
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

  // A 2D isometric game: positions are tiles and height levels on the grid,
  // drawn by the engine's one renderer.
  auto& render = addSystem<sfs::RenderSystem3D>(renderer3D());
  render.setSpace2D(isometricSpace());
  sfs::ShadowSettings3D shadows;
  shadows.enabled = false;
  shadows.pointLights = 0;
  render.setShadowSettings(shadows);
  render.withModule<sfs::SpriteModule>(m_assetStore);
  render.withModule<sfs::VoxelModule>(
      m_assetStore, voxelWorld, m_blockRegistry);
  auto& decals = render.withModule<sfs::DecalModule>(m_assetStore);
  auto& particles = render.withModule<sfs::ParticleModule>(m_assetStore);
  sfs::registerBloodEffects(particles);
  sfs::registerBloodEffects(particles,
                            "ichor",
                            glm::vec3{0.15f, 0.55f, 1.0f},
                            glm::vec3{0.0f, 0.08f, 0.35f});
  particles.registerEffect("embers", sfs::emberEffect());
  particles.enableStains(decals, &voxelWorld);
  render.withModule<sfs::UiModule>(*this);

  auto& sun = addSystem<SunController>();

  if (sfs::LuaScripting* lua = sfs::activeLua())
    lua->registerConfig(sun);

  // Same surface drives movement so actors are blocked by cliffs (step up to
  // one level, slide along anything taller).
  getSystem<sfs::MovementSystem>().setTerrainHeightSource(&voxelWorld);
}

void GameScene::createEntities()
{
  m_player = &createObject<Player>();
  createObject<GameHud>();

  auto& emberLamp =
      createObject<Lamp>(glm::vec2{16.5, 16.5}, Lamp::Color::Pink);
  emberLamp.entity().addComponent<sfs::ParticleEmitterComponent>(
      "embers", glm::vec3{0.0f, 0.0f, 0.4f});

  createObject<Lamp>(glm::vec2{16.5, 11.5}, Lamp::Color::Moonlight);
  createObject<Lamp>(glm::vec2{5.5, 13.5});
  createObject<Lamp>(glm::vec2{6.5, 13.5});
}

bool GameScene::pickTile(glm::vec2 pixel, TilePick& pick)
{
  // Under the mouse, from as high as the terrain goes on down: the first tile
  // whose ground is at the height looked through.
  auto& render = getSystem<sfs::RenderSystem3D>();
  auto& voxels = getSystem<sfs::VoxelWorld>();
  const glm::vec2 world(render.screenRay(pixel).origin);
  const sfs::Space2D& space = *render.space2D();
  for (int level = kHighestLevel; level >= 0; --level)
  {
    const glm::vec2 at = space.unplace(world, static_cast<float>(level));
    const glm::ivec2 tile(glm::floor(at));
    if (voxels.terrainHeightAt(tile.x, tile.y) == level)
    {
      pick = {tile, level, at};
      return true;
    }
  }
  return false;
}

void GameScene::onProcessInput(const sfs::Input& input)
{
  TilePick pick;
  m_hasHoveredTile = pickTile(input.mouse().getPosition(), pick);
  if (!m_hasHoveredTile)
    return;
  m_hovered = pick;

  if (m_player && input.mouse().mouseHeld(sfs::MouseButton::Right))
  {
    const glm::vec2 from =
        m_player->entity().getComponent<sfs::TransformComponent>().position2D();
    glm::vec2 dir = pick.point - from;
    const float len = glm::length(dir);
    dir = len > 0.0001f ? dir / len : glm::vec2{1.0f, 0.0f};

    // Layered shotgun gore blast along the shot direction.
    spawnGore(*getSystem<sfs::RenderSystem3D>().module<sfs::ParticleModule>(),
              pick.point,
              static_cast<float>(pick.level),
              dir,
              12.0f,
              "blood");
  }

  if (input.mouse().mouseHeld(sfs::MouseButton::Left))
  {
    const int z = (pick.level - 1) / sfs::kLevelsPerBlock;
    getSystem<sfs::VoxelWorld>().setBlock(
        pick.tile.x, pick.tile.y, z, sfs::kAirBlock);
  }
}

sfs::ui::View GameHud::body()
{
  using namespace sfs::ui;
  auto& scene = static_cast<GameScene&>(this->scene());
  const SunController* sun = scene.tryGetSystem<SunController>();
  return VStack(
             Alignment::Leading,
             {
                 Text("FPS: " + std::to_string(static_cast<int>(scene.fps()))),
                 Text("Time: " + (sun ? sun->timeString12Hour() : "")),
             })
      .padding(20)
      .align(Alignment::TopLeading);
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

  if (m_player && m_player->entity().hasComponent<sfs::PointLightComponent>())
  {
    auto& light = m_player->entity().getComponent<sfs::PointLightComponent>();

    ImGui::SetNextWindowSize(ImVec2(300, 180), ImGuiCond_FirstUseEver);
    ImGui::Begin("Player Light");

    // World units: tiles across, elevation levels up.
    ImGui::SliderFloat("Radius (tiles)", &light.radius, 0.0f, 32.0f);
    ImGui::SliderFloat("Strength", &light.intensity, 0.0f, 4.0f);
    ImGui::SliderFloat("Height (levels)", &light.offset.z, 0.0f, 16.0f);
    ImGui::ColorEdit3("Color", &light.color.x);

    ImGui::End();
  }
#endif
}
