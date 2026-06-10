#include "scenes/voxel3dScene.h"

#include "config.h"
#include "tinyHeightmapGenerator.h"

#include "engine/core/util/profiling.h"
#include "engine/runtime/TextRenderer/textRenderer.h"
#include "engine/runtime/input/input.h"
#include "engine/runtime/systems/voxel3DRenderSystem.h"
#include "engine/runtime/voxel/tinyVoxelWorld.h"
#include "engine/runtime/voxel/voxelFireSystem.h"
#include "engine/runtime/voxel/waterSurfaceSystem.h"
#include "glm/glm/common.hpp"
#include "glm/glm/exponential.hpp"
#include "glm/glm/ext/vector_int3.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"

#include <string>

#ifndef ENGINE_WEB
  #include <imgui.h>
#endif

namespace
{
// Voxels per block -- the shared scale unit. Speeds/sizes are keyed off it so
// the game feels the same (in blocks) at any resolution.
constexpr int kVPB = TinyHeightmapGenerator::kVPB;

constexpr float kMoveSpeed = kVPB * 6.25f; // ~6 blocks / second
constexpr float kYawSpeed = 1.7f;          // radians / second
constexpr float kZoomSpeed = kVPB * 7.0f;
// A 1-block-tall, ~0.4-block-wide figure (matches the iso game's 1-block
// player).
constexpr float kPlayerHalfY = kVPB * 0.5f;
constexpr float kPlayerHalfXZ = kVPB * 0.2f;

// Left-click explosion: crater radius + how hard debris is flung (in voxels).
constexpr int kExplodeRadius = kVPB * 2; // ~2-block crater
constexpr float kExplodePower = kVPB * 8.0f;

// How fast the player's height eases to the terrain (1/sec). Lower = smoother
// but floatier over steps; this keeps the camera from snapping on bumpy ground.
constexpr float kVerticalFollow = 10.0f;

// Camera default + zoom range (ortho half-height, in voxels). Max zoom is
// capped to what the load radius covers, so zooming out never reveals the
// world's edge.
constexpr float kZoomDefault = kVPB * 6.0f;
constexpr float kZoomMin = kVPB * 1.0f;
constexpr float kZoomMax = kVPB * 8.0f;

// Streaming world: Y chunk band (covers the ~10-block max terrain height) + a
// load radius (chunks) wider than the camera frames. The radius exceeds the
// max-zoom view by a margin so chunks generate OFF-SCREEN (no visible pop-in);
// memory scales with radius^2.
constexpr int kMinChunkY = 0;
constexpr int kMaxChunkY = 12 * kVPB / 32; // ~10-block max terrain + margin
constexpr int kStreamRadius = 12;
} // namespace

void Voxel3DScene::onInit()
{
  // The world owns storage + streaming; the renderer just draws it. The
  // generator is the game's terrain shape (heightmap), injected into the world.
  auto& world =
      addSystem<sfs::TinyVoxelWorld>(kMinChunkY, kMaxChunkY, kStreamRadius);
  world.setGenerator(&m_gen);
  m_world = &world;

  // Fire CA runs before the renderer so its burning voxels are current when the
  // renderer emits flame particles from them.
  auto& fire = addSystem<sfs::VoxelFireSystem>(&world);
  m_fire = &fire;

  // Volumetric water (coarse grid): flows + levels before the renderer meshes
  // its surface, which it reads from this system.
  auto& water = addSystem<sfs::WaterSurfaceSystem>(&world);
  water.setSeaLevel(m_gen.seaLevel());
  m_water = &water;

  auto& render =
      addSystem<sfs::Voxel3DRenderSystem>(WINDOW_WIDTH, WINDOW_HEIGHT);
  render.setWorld(&world);
  render.setFire(&fire);
  render.setWaterSurface(&water);
  m_render = &render;

  // Spawn on land near the origin (walk +x off any lake so the player isn't
  // submerged at start). The generator answers height without waiting on
  // stream.
  const int sea = m_gen.seaLevel();
  int px = 0;
  const int pz = 0;
  for (int r = 0; r < 4096 && m_gen.terrainHeight(px, pz) < sea;
       r += TinyHeightmapGenerator::kVPB)
    px = r;
  m_playerPos =
      glm::vec3{static_cast<float>(px),
                static_cast<float>(m_gen.terrainHeight(px, pz)) + kPlayerHalfY,
                static_cast<float>(pz)};

  world.setFocus(m_playerPos); // first stream loads around the spawn
  render.camera().focus = m_playerPos;
  render.camera().zoom = kZoomDefault;
  applySun();
  render.setPlayerBox(m_playerPos,
                      glm::vec3{kPlayerHalfXZ, kPlayerHalfY, kPlayerHalfXZ},
                      glm::vec3{0.92f, 0.26f, 0.2f});
  updatePlayerLight();
}

void Voxel3DScene::updatePlayerLight()
{
  // A point light carried by the player (like the sample's lamp); params are
  // editable in the debug panel.
  sfs::Voxel3DRenderSystem::PointLight light;
  light.pos = m_playerPos;
  light.color = m_lightColor;
  light.radius = m_lightRadius;
  light.intensity = m_lightIntensity;
  m_render->setLights({light});
}

void Voxel3DScene::applySun()
{
  // Ported from the sample's SunController: arc the sun across the sky and fade
  // ambient/colour from night to day. t: 0 midnight, .25 sunrise, .5 noon.
  const float t = m_timeOfDay;
  const auto sat = [](float x) { return glm::clamp(x, 0.0f, 1.0f); };
  const auto ramp = [&](float a, float b, float x)
  { return sat((x - a) / (b - a)); };

  const float dayProgress = ramp(6.0f / 24.0f, 18.0f / 24.0f, t);
  const float sunX = glm::cos(dayProgress * 3.14159265f);
  const float noon = sat(1.0f - glm::abs(dayProgress * 2.0f - 1.0f));
  const float smoothNoon = noon * noon * (3.0f - 2.0f * noon);
  const float sunZ = 0.08f + smoothNoon * 0.91f;

  const float morning = ramp(5.0f / 24.0f, 7.0f / 24.0f, t);
  const float evening = 1.0f - ramp(17.0f / 24.0f, 19.0f / 24.0f, t);
  const float daylight = sat(morning * evening);

  glm::vec2 hdir{sunX, -0.65f};
  hdir = glm::length(hdir) > 0.001f ? glm::normalize(hdir) : glm::vec2{0, -1};
  const float hAmount = glm::sqrt(glm::max(0.0f, 1.0f - sunZ * sunZ));
  const glm::vec3 sunDir =
      glm::normalize(glm::vec3{hdir.x * hAmount, sunZ, hdir.y * hAmount});

  const glm::vec3 nightColor{0.10f, 0.16f, 0.34f}; // moonlit blue
  const glm::vec3 dayColor{1.0f, 1.0f, 1.0f};
  const glm::vec3 duskColor{1.0f, 0.62f, 0.35f};
  const float dusk = ramp(16.5f / 24.0f, 18.5f / 24.0f, t);
  const glm::vec3 dayTint = glm::mix(dayColor, duskColor, dusk);

  const glm::vec3 color = glm::mix(nightColor, dayTint, daylight);
  const float ambient = 0.12f + daylight * 0.88f;
  const float diffuse = daylight * glm::mix(0.65f, 0.30f, noon);

  m_render->setSun(sunDir, color, ambient, diffuse);
}

void Voxel3DScene::onProcessInput(const sfs::Input& input)
{
  const auto& kb = input.keyboard();

  m_moveInput = glm::vec2{0.0f, 0.0f};
  if (kb.keyHeld(sfs::Key::W))
    m_moveInput.y += 1.0f;
  if (kb.keyHeld(sfs::Key::S))
    m_moveInput.y -= 1.0f;
  if (kb.keyHeld(sfs::Key::D))
    m_moveInput.x += 1.0f;
  if (kb.keyHeld(sfs::Key::A))
    m_moveInput.x -= 1.0f;

  m_yawInput = (kb.keyHeld(sfs::Key::E) ? 1.0f : 0.0f) -
               (kb.keyHeld(sfs::Key::Q) ? 1.0f : 0.0f);
  m_zoomInput = (kb.keyHeld(sfs::Key::R) ? 1.0f : 0.0f) - // R = zoom in
                (kb.keyHeld(sfs::Key::F) ? 1.0f : 0.0f);  // F = zoom out

  // Left-click: blow a crater in the terrain. Right-click: ignite (start a
  // fire).
  const auto pickNdc = [&](const glm::vec2& mp)
  {
    return glm::vec2{2.0f * mp.x / static_cast<float>(WINDOW_WIDTH) - 1.0f,
                     1.0f - 2.0f * mp.y / static_cast<float>(WINDOW_HEIGHT)};
  };
  if (m_render && input.mouse().mousePressed(sfs::MouseButton::Left))
  {
    glm::ivec3 hit;
    if (m_render->raycastVoxel(pickNdc(input.mouse().getPosition()), hit))
    {
      // Carving clears terrain; the water sim flows into the new cavity over
      // the next ticks.
      m_render->explode(hit, kExplodeRadius, kExplodePower);
    }
  }
  if (m_render && m_fire && input.mouse().mousePressed(sfs::MouseButton::Right))
  {
    glm::ivec3 hit;
    if (m_render->raycastVoxel(pickNdc(input.mouse().getPosition()), hit))
      m_fire->ignite(hit);
  }
}

void Voxel3DScene::onUpdate(double deltaTime)
{
  if (deltaTime > 0.0001)
    m_fps += (1.0 / deltaTime - m_fps) * 0.1; // eased FPS readout

  if (!m_render)
    return;

  if (m_sunEnabled && deltaTime > 0.0)
  {
    m_timeOfDay +=
        static_cast<float>(deltaTime) / m_dayLengthSeconds * m_timeMultiplier;
    m_timeOfDay -= glm::floor(m_timeOfDay);
  }
  applySun();

  const float dt = static_cast<float>(deltaTime);
  sfs::OrthoOrbitCamera& cam = m_render->camera();

  cam.yaw += m_yawInput * kYawSpeed * dt;
  cam.zoom =
      glm::clamp(cam.zoom - m_zoomInput * kZoomSpeed * dt, kZoomMin, kZoomMax);

  glm::vec3 move = cam.forward() * m_moveInput.y + cam.right() * m_moveInput.x;
  if (glm::length(move) > 0.0001f)
  {
    move = glm::normalize(move);
    m_playerPos.x += move.x * kMoveSpeed * dt;
    m_playerPos.z += move.z * kMoveSpeed * dt;
  }
  // Ease the player's height toward the terrain instead of snapping each frame,
  // so walking over the bumpy surface glides (the camera follows the player, so
  // a per-frame snap jerked the whole view).
  const float targetY = static_cast<float>(m_gen.terrainHeight(
                            static_cast<int>(glm::floor(m_playerPos.x)),
                            static_cast<int>(glm::floor(m_playerPos.z)))) +
                        kPlayerHalfY;
  m_playerPos.y +=
      (targetY - m_playerPos.y) * (1.0f - glm::exp(-kVerticalFollow * dt));

  if (m_world)
    m_world->setFocus(m_playerPos); // stream chunks around the player

  cam.focus = m_playerPos;
  m_render->setPlayerBox(m_playerPos,
                         glm::vec3{kPlayerHalfXZ, kPlayerHalfY, kPlayerHalfXZ},
                         glm::vec3{0.92f, 0.26f, 0.2f});
  updatePlayerLight();

  FrameMark; // Tracy frame boundary
}

void Voxel3DScene::onRender()
{
  textRenderer().drawText(
      20, 20, "FPS: " + std::to_string(static_cast<int>(m_fps)));
}

void Voxel3DScene::onDebugUI()
{
#ifndef ENGINE_WEB
  ImGui::SetNextWindowSize(ImVec2(320, 160), ImGuiCond_FirstUseEver);
  ImGui::Begin("Time of Day");
  ImGui::Text("FPS: %d", static_cast<int>(m_fps));
  float hour = m_timeOfDay * 24.0f;
  if (ImGui::SliderFloat("Hour", &hour, 0.0f, 24.0f))
    m_timeOfDay = hour / 24.0f;
  ImGui::Checkbox("Run cycle", &m_sunEnabled);
  ImGui::SliderFloat("Day speed", &m_timeMultiplier, 0.0f, 20.0f);
  ImGui::SliderFloat("Day length (s)", &m_dayLengthSeconds, 5.0f, 600.0f);
  ImGui::End();

  ImGui::SetNextWindowSize(ImVec2(320, 160), ImGuiCond_FirstUseEver);
  ImGui::Begin("Player Light");
  ImGui::SliderFloat("Radius", &m_lightRadius, 0.0f, 400.0f);
  ImGui::SliderFloat("Intensity", &m_lightIntensity, 0.0f, 6.0f);
  ImGui::ColorEdit3("Color", &m_lightColor.x);
  ImGui::End();
#endif
}
