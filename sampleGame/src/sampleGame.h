
#pragma once

#include <SDL_events.h>
#include <engine/game/game.h>

class SampleGame : public sfs::Game
{
public:
  SampleGame() = default;
  ~SampleGame() = default;

protected:
  void onSetup() override;
  void onRender() override;
  void onProcessInput(const sfs::Input& input) override;
  void onDestroy() override;
};
