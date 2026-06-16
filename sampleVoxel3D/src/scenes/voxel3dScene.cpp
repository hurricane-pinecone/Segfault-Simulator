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
  m_carveDir = (kb.keyHeld(sfs::Key::Up) ? 1.0f : 0.0f) -
               (kb.keyHeld(sfs::Key::Down) ? 1.0f : 0.0f);

  const sfs::MouseInput& mouse = input.mouse();
  m_scrollY = static_cast<float>(mouse.getScrollY());
  m_editMode = mouse.mouseHeld(sfs::MouseButton::Left)    ? 1
               : mouse.mouseHeld(sfs::MouseButton::Right) ? 2
                                                          : 0;
  if (kb.keyHeld(
          sfs::Key::F)) // hold F to emit smoke at the cursor (like water)
    m_editMode = 3;
  if (kb.keyHeld(sfs::Key::R)) // hold R to drop rubble (powder) at the cursor
    m_editMode = 4;
  if (kb.keyHeld(
          sfs::Key::C)) // hold C to ignite flammable voxels at the cursor
    m_editMode = 5;
  if (kb.keyPressed(sfs::Key::P)) // P toggles the debug wireframe overlays
    m_debugWire = !m_debugWire;
  m_explode = kb.keyPressed(sfs::Key::X); // X detonates at the cursor
  const glm::vec2 p = mouse.getPosition();
  m_mouseX = p.x;
  m_mouseY = p.y;
}

void Voxel3DScene::onUpdate(double deltaTime)
{
  if (!m_voxel)
    return;
  m_voxel->rotate(m_yawDir * 1.3f * static_cast<float>(deltaTime));
  if (m_scrollY != 0.0f)
    m_voxel->zoom(m_scrollY);
  m_voxel->adjustCarveRadius(m_carveDir * 12.0f *
                             static_cast<float>(deltaTime));
  m_voxel->setDebugWire(m_debugWire);
  m_voxel->setEdit(m_editMode, m_mouseX, m_mouseY);
  if (m_explode)
    m_voxel->requestExplosion();
}
