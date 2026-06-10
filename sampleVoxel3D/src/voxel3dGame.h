#pragma once

#include "engine/runtime/game/game.h"
#include "engine/runtime/input/input.h"

#include <memory>

// The tiny-voxel 3D app. Runs on the WebGPU render backend and loads the
// brickmap voxel scene; the window title shows fps + GPU frame time.
class Voxel3DGame : public sfs::Game
{
protected:
  std::unique_ptr<sfs::IRenderBackend> makeRenderBackend() override;
  void onSetup() override;
  void onUpdate(double deltaTime) override;
  void onProcessInput(const sfs::Input& input) override;
  void onDestroy() override {}

private:
  float m_hudAccum = 0.0f;
  int m_hudFrames = 0;
};
