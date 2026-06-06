#pragma once

#include "engine/core/rendering/projection/flatProjection.h"
#include "engine/runtime/game/game.h"
#include "engine/runtime/input/input.h"

class PlatformerScene;

/**
 * The platformer app. Uses the engine's DEFAULT renderer (the flat
 * OpenGLQuadRenderer) -- it does NOT override createQuadRenderer, so there is no
 * isometric backend involved. Each frame it builds a FlatProjection centred on
 * the player and hands it to the scene's FlatRenderSystem (camera follow).
 */
class PlatformerGame : public sfs::Game
{
protected:
  void onSetup() override;
  void onUpdate(double deltaTime) override;
  void onProcessInput(const sfs::Input& input) override;
  void onDestroy() override {}

private:
  PlatformerScene* m_scene = nullptr;
  sfs::FlatProjection m_projection;
};
