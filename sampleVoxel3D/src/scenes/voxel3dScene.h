#pragma once

#include "engine/runtime/sceneManager/scene.h"

namespace sfs
{
class WebGpuRenderBackend;
class VoxelGpuSystem;
} // namespace sfs

// Wires the WebGPU brickmap voxel world into a scene. Adds the VoxelGpuSystem
// (which owns the GPU world + raymarch renderer) and feeds it input: Q/E orbit
// the camera, held left/right mouse carve / spawn water.
class Voxel3DScene : public sfs::Scene
{
public:
  Voxel3DScene(sfs::SceneId id,
               const sfs::SceneServices& services,
               sfs::WebGpuRenderBackend& backend);

protected:
  void onInit() override;
  void onProcessInput(const sfs::Input& input) override;
  void onUpdate(double deltaTime) override;

private:
  sfs::WebGpuRenderBackend& m_backend;
  sfs::VoxelGpuSystem* m_voxel = nullptr;

  float m_yawDir = 0.0f; // -1 / 0 / +1 from Q/E
  int m_editMode = 0;    // 0 none, 1 carve, 2 spawn water
  float m_mouseX = 0.0f;
  float m_mouseY = 0.0f;
};
