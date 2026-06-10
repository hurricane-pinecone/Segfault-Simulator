#include "voxel3dGame.h"

#include "scenes/voxel3dScene.h"

#include "engine/webgpu/voxel/voxelGpuSystem.h"
#include "engine/webgpu/webGpuRenderBackend.h"

#include <SDL_video.h>

#include <cstdio>

std::unique_ptr<sfs::IRenderBackend> Voxel3DGame::makeRenderBackend()
{
  return std::make_unique<sfs::WebGpuRenderBackend>();
}

void Voxel3DGame::onSetup()
{
  auto* backend = static_cast<sfs::WebGpuRenderBackend*>(renderBackend());
  sceneManager.createScene<Voxel3DScene>(*backend);
}

void Voxel3DGame::onProcessInput(const sfs::Input& input)
{
  if (input.keyboard().keyPressed(sfs::Key::Escape))
    isRunning = false;
}

void Voxel3DGame::onUpdate(double deltaTime)
{
  m_hudAccum += static_cast<float>(deltaTime);
  ++m_hudFrames;
  if (m_hudAccum < 0.4f)
    return;

  const float fps = static_cast<float>(m_hudFrames) / m_hudAccum;
  sfs::Scene* scene = sceneManager.current();
  char title[160];
  if (scene && scene->hasSystem<sfs::VoxelGpuSystem>())
  {
    const sfs::VoxelGpuSystem& sys = scene->getSystem<sfs::VoxelGpuSystem>();
    if (sys.hasTimestamps())
      std::snprintf(title,
                    sizeof(title),
                    "sampleVoxel3D -- %.0f fps (vsync) | GPU %.2f ms = sim "
                    "%.2f + render %.2f  [512^3]",
                    fps,
                    sys.gpuTotalMs(),
                    sys.gpuSimMs(),
                    sys.gpuRenderMs());
    else
      std::snprintf(
          title, sizeof(title), "sampleVoxel3D -- %.0f fps  [512^3]", fps);
    SDL_SetWindowTitle(window, title);
  }
  m_hudAccum = 0.0f;
  m_hudFrames = 0;
}
