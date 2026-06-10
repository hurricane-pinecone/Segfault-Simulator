#include "engine/webgpu/voxel/voxelGpuSystem.h"

#include "engine/webgpu/webGpuRenderBackend.h"

#include "SDL.h"

namespace sfs
{

VoxelGpuSystem::VoxelGpuSystem(WebGpuRenderBackend& backend)
    : m_backend(backend)
{
  m_world = std::make_unique<GpuVoxelWorld>(backend.context());
  m_world->generate();
}

void VoxelGpuSystem::render()
{
  WGPUCommandEncoder enc = m_backend.currentEncoder();
  WGPUTextureView view = m_backend.currentView();
  if (!enc || !view) // surface was lost this frame
    return;

  const int width = m_backend.width();
  const int height = m_backend.height();
  const float time = static_cast<float>(SDL_GetTicks()) / 1000.0f;

  float cam16[16];
  m_camera.packUniform(cam16, width, height, time);

  GpuVoxelWorld::EditCmd edit;
  const GpuVoxelWorld::EditCmd* editPtr = nullptr;
  if (m_editMode != 0)
  {
    glm::vec3 origin;
    glm::vec3 dir;
    m_camera.pickRay(width, height, m_mouseX, m_mouseY, origin, dir);
    edit.origin[0] = origin.x;
    edit.origin[1] = origin.y;
    edit.origin[2] = origin.z;
    edit.dir[0] = dir.x;
    edit.dir[1] = dir.y;
    edit.dir[2] = dir.z;
    edit.mode = m_editMode;
    edit.radius = m_editMode == 1 ? 12.0f : 8.0f;
    editPtr = &edit;
  }

  m_world->setDebugMouse(m_mouseX, m_mouseY);
  m_world->recordFrame(enc, view, cam16, editPtr, m_frame++);
}

} // namespace sfs
