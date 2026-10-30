#include "platformerGame.h"

#include "scenes/platformerScene.h"

#include "engine/webgpu/webGpuRenderBackend.h"

void PlatformerGame::onSetup()
{
  m_scene = sceneManager.createScene<PlatformerScene>("Platformer");
  isRunning = true;
}

std::unique_ptr<sfs::IRenderBackend> PlatformerGame::makeRenderBackend()
{
  return std::make_unique<sfs::WebGpuRenderBackend>();
}

void PlatformerGame::onProcessInput(const sfs::Input& input)
{
  if (input.keyboard().keyPressed(sfs::Key::Escape))
  {
#ifndef ENGINE_WEB
    isRunning = false; // on web, Esc must not kill the tab's game loop
#endif
  }
}
