#include "voxel3dGame.h"

#include "scenes/voxel3dScene.h"

void Voxel3DGame::onSetup()
{
  sceneManager.createScene<Voxel3DScene>("Voxel3D");
  isRunning = true;
}

void Voxel3DGame::onProcessInput(const sfs::Input& input)
{
  if (input.keyboard().keyPressed(sfs::Key::Escape))
  {
#ifndef ENGINE_WEB
    isRunning = false;
#endif
  }
}
