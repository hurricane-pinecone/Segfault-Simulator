
#pragma once

#include "engine/ecs/entity.h"
#include <SDL_events.h>
#include <engine/game/game.h>

class SampleGame : public sfs::Game
{
public:
  ~SampleGame() = default;

protected:
  void onSetup() override;
  void onProcessInput(SDL_Event& event) override;
  void onDestroy() override;

private:
  void loadMap();

private:
  sfs::Entity player;
};
