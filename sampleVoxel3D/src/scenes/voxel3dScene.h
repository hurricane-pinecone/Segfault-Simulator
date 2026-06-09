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

private:
  int terrainHeight(int wx, int wz) const; // top solid voxel (world y)
  std::uint32_t voxelAt(int wx, int wy, int wz) const; // colour or 0 (air)

  sfs::Voxel3DRenderSystem* m_render = nullptr;
  sfs::Noise m_noise;  // macro hills (block scale)
  sfs::Noise m_detail; // fine sub-block surface detail

  glm::vec3 m_playerPos{0.0f, 0.0f, 0.0f};
  glm::vec2 m_moveInput{0.0f, 0.0f}; // x = strafe, y = forward
  float m_yawInput = 0.0f;
  float m_zoomInput = 0.0f;
};
