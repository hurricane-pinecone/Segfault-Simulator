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

  // Queue a continuous edit at a screen pixel for the next frame: mode 1 carve,
  // mode 2 spawn water, 0 none.
  void setEdit(int mode, float mouseX, float mouseY)
  {
    m_editMode = mode;
    m_mouseX = mouseX;
    m_mouseY = mouseY;
  }

  bool hasTimestamps() const { return m_world && m_world->hasTimestamps(); }
  double gpuSimMs() const { return m_world ? m_world->gpuSimMs() : 0.0; }
  double gpuRenderMs() const { return m_world ? m_world->gpuRenderMs() : 0.0; }
  double gpuTotalMs() const { return m_world ? m_world->gpuTotalMs() : 0.0; }

protected:
  void render() override;

private:
  WebGpuRenderBackend& m_backend;
  std::unique_ptr<GpuVoxelWorld> m_world;
  OrbitCamera m_camera{static_cast<float>(GpuVoxelWorld::kWorld)};

  uint32_t m_frame = 0;
  int m_editMode = 0;
  float m_mouseX = 0.0f;
  float m_mouseY = 0.0f;
};

} // namespace sfs
