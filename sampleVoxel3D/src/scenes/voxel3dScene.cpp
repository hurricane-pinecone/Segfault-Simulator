#include "scenes/voxel3dScene.h"

#include "engine/runtime/input/input.h"
#include "engine/webgpu/voxel/voxelGpuSystem.h"
#include "engine/webgpu/webGpuRenderBackend.h"

Voxel3DScene::Voxel3DScene(sfs::SceneId id,
                           const sfs::SceneServices& services,
                           sfs::WebGpuRenderBackend& backend)
    : sfs::Scene(id, services, "voxel3d"), m_backend(backend)
{
}

void Voxel3DScene::onInit()
{
  m_voxel = &addSystem<sfs::VoxelGpuSystem>(m_backend);
}

void Voxel3DScene::onProcessInput(const sfs::Input& input)
{
  const sfs::KeyboardInput& kb = input.keyboard();
  m_yawDir = (kb.keyHeld(sfs::Key::E) ? 1.0f : 0.0f) -
             (kb.keyHeld(sfs::Key::Q) ? 1.0f : 0.0f);

  const sfs::MouseInput& mouse = input.mouse();
  m_editMode = mouse.mouseHeld(sfs::MouseButton::Left)    ? 1
               : mouse.mouseHeld(sfs::MouseButton::Right) ? 2
                                                          : 0;
  const glm::vec2 p = mouse.getPosition();
  m_mouseX = p.x;
  m_mouseY = p.y;
}

void Voxel3DScene::onUpdate(double deltaTime)
{
  if (!m_voxel)
    return;
  m_voxel->rotate(m_yawDir * 1.3f * static_cast<float>(deltaTime));
  m_voxel->setEdit(m_editMode, m_mouseX, m_mouseY);
}
