
#pragma once

#include <SDL_events.h>
#include <engine/game/game.h>
#include <engine/rendering/iQuadRenderer.h>
#include <engine/rendering/util/isometric/geometry.h>
#include <memory>

class SampleGame : public sfs::Game
{
public:
  SampleGame() = default;
  ~SampleGame() = default;

protected:
  void onSetup() override;
  void onProcessInput(const sfs::Input& input) override;
  void onUpdate(double deltaTime) override;
  void onDestroy() override;

  std::unique_ptr<sfs::IQuadRenderer>
  createQuadRenderer(int windowWidth, int windowHeight) override;

private:
  sfs::IsometricProjectionConfig m_isoConfig;
  sfs::IsometricProjection m_isoProjection;
};
