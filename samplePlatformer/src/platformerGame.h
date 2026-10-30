#pragma once

#include "engine/runtime/game/game.h"

class PlatformerScene;

/**
 * The platformer app: one scene, drawn through the engine's renderer (see
 * PlatformerScene).
 */
class PlatformerGame : public sfs::Game
{
protected:
  void onSetup() override;
  void onProcessInput(const sfs::Input& input) override;
  void onDestroy() override {}
  std::unique_ptr<sfs::IRenderBackend> makeRenderBackend() override;

private:
  PlatformerScene* m_scene = nullptr;
};
