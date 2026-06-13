#pragma once

#include "engine/runtime/sceneManager/scene.h"

namespace sfs
{
class WebGpuRenderBackend;
class VoxelGpuSystem;
} // namespace sfs

// Wires the WebGPU brickmap voxel world into a scene. Adds the VoxelGpuSystem
// (which owns the GPU world + raymarch renderer) and feeds it input: Q/E orbit
// the camera, mouse wheel zoom, up/down arrows resize the carve (and water
// spawn), held left/right mouse carve / spawn water.
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

  float m_yawDir = 0.0f;   // -1 / 0 / +1 from Q/E
  float m_scrollY = 0.0f;  // mouse wheel this frame
  float m_carveDir = 0.0f; // -1 / 0 / +1 from down/up arrows
  int m_editMode = 0;      // 0 none, 1 carve, 2 spawn water
  float m_mouseX = 0.0f;
  float m_mouseY = 0.0f;
  bool m_debugWire = true; // P toggles brick-grid + body-box overlays
  bool m_explode = false;  // X detonates an explosion at the cursor this frame
};
