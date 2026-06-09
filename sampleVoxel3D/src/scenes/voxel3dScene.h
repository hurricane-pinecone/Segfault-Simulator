#pragma once

#include "engine/core/noise/noise.h"
#include "engine/runtime/sceneManager/scene.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <cstdint>

namespace sfs
{
class Voxel3DRenderSystem;
}

// Builds a fixed patch of tiny coloured voxels (noise heightmap:
// stone/dirt/grass
// + water) and lets you drive a player box around it while orbiting + zooming
// the orthographic isometric camera. Proves the 3D render path + base-builder
// camera.
class Voxel3DScene : public sfs::Scene
{
public:
  using sfs::Scene::Scene;

protected:
  void onInit() override;
  void onUpdate(double deltaTime) override;
  void onProcessInput(const sfs::Input& input) override;
  void onRender() override;  // FPS overlay for gauging perf
  void onDebugUI() override; // ImGui panel (time of day, player light)

private:
  int terrainHeight(int wx, int wz) const; // top solid voxel (world y)
  std::uint32_t voxelAt(int wx, int wy, int wz) const; // colour or 0 (air)
  void updatePlayerLight(); // light follows the player
  void applySun(); // sun dir/colour/ambient from m_timeOfDay -> render system

  sfs::Voxel3DRenderSystem* m_render = nullptr;
  sfs::Noise m_noise;  // macro hills (block scale)
  sfs::Noise m_detail; // fine sub-block surface detail

  glm::vec3 m_playerPos{0.0f, 0.0f, 0.0f};
  glm::vec2 m_moveInput{0.0f, 0.0f}; // x = strafe, y = forward
  float m_yawInput = 0.0f;
  float m_zoomInput = 0.0f;
  double m_fps = 0.0;

  // Day/night (0 = midnight, .25 sunrise, .5 noon, .75 sunset).
  float m_timeOfDay = 0.34f;
  float m_dayLengthSeconds = 90.0f;
  float m_timeMultiplier = 1.0f;
  bool m_sunEnabled = true;

  // Player point light (editable in the debug panel).
  glm::vec3 m_lightColor{1.0f, 0.82f, 0.55f};
  float m_lightRadius = 150.0f;
  float m_lightIntensity = 2.2f;
};
