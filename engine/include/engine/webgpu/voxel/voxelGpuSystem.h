#pragma once

#include "engine/core/ecs/system.h"
#include "engine/webgpu/voxel/gpuVoxelWorld.h"
#include "engine/webgpu/voxel/orbitCamera.h"

#include <memory>

namespace sfs
{

class WebGpuRenderBackend;

// Drives the WebGPU brickmap voxel world: owns the GPU world + orbit camera,
// and each frame records the edit / water / recount / raymarch passes into the
// backend's command encoder. The pass ordering lives here (one system), not
// across sibling systems. The scene feeds it camera rotation and click edits.
class VoxelGpuSystem : public System
{
public:
  explicit VoxelGpuSystem(WebGpuRenderBackend& backend);

  // Rotate the orbit camera (Q/E).
  void rotate(float deltaYaw) { m_camera.addYaw(deltaYaw); }

  // Zoom the orbit camera (mouse wheel).
  void zoom(float scrollY) { m_camera.addZoom(scrollY); }

  // Grow/shrink the carve (and water-spawn) radius (up/down arrows).
  void adjustCarveRadius(float delta)
  {
    m_carveRadius += delta;
    if (m_carveRadius < 0.0f) // radius 0 -> i32(R)=0 -> a single-voxel carve
      m_carveRadius = 0.0f;
    if (m_carveRadius > 24.0f)
      m_carveRadius = 24.0f;
  }

  // Queue a continuous edit at a screen pixel for the next frame: mode 1 carve,
  // mode 2 spawn water, 0 none.
  void setEdit(int mode, float mouseX, float mouseY)
  {
    m_editMode = mode;
    m_mouseX = mouseX;
    m_mouseY = mouseY;
  }

  // Toggle the debug wireframe overlays (brick grid + body box).
  void setDebugWire(bool on)
  {
    if (m_world)
      m_world->setDebugWire(on);
  }

  // Detonate an explosion at the cursor on the next frame (X key for now).
  void requestExplosion() { m_explodeRequested = true; }

  bool hasTimestamps() const { return m_world && m_world->hasTimestamps(); }
  double gpuSimMs() const { return m_world ? m_world->gpuSimMs() : 0.0; }
  double gpuRenderMs() const { return m_world ? m_world->gpuRenderMs() : 0.0; }
  double gpuTotalMs() const { return m_world ? m_world->gpuTotalMs() : 0.0; }

protected:
  void render() override;
  void update(double dt) override
  {
    if (m_world)
      m_world->stepBody(dt);
  }

private:
  WebGpuRenderBackend& m_backend;
  std::unique_ptr<GpuVoxelWorld> m_world;
  OrbitCamera m_camera{static_cast<float>(GpuVoxelWorld::kWorld)};

  uint32_t m_frame = 0;
  int m_editMode = 0;
  float m_mouseX = 0.0f;
  float m_mouseY = 0.0f;
  float m_carveRadius = 3.6f; // carve sphere radius; water spawn matches it
  bool m_explodeRequested = false;
  float m_blastRadius = 20.0f; // explosion crater radius (voxels)
  float m_blastForce = 350.0f; // explosion impulse + debris eject strength
};

} // namespace sfs
