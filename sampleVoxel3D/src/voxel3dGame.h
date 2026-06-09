#pragma once

#include "engine/runtime/game/game.h"
#include "engine/runtime/input/input.h"

// The tiny-voxel 3D POC app. Uses the engine's DEFAULT quad renderer (unused --
// the scene draws through its own Voxel3DRenderSystem) and just loads the 3D
// scene.
class Voxel3DGame : public sfs::Game
{
protected:
  void onSetup() override;
  void onProcessInput(const sfs::Input& input) override;
  void onDestroy() override {}
};
