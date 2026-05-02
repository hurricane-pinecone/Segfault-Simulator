
#pragma once

#include "engine/ecs/entity.h"
#include <engine/game/game.h>

class SampleGame : public Game
{
public:
  ~SampleGame() = default;

protected:
  void onSetup() override;
  void onDestroy() override;

private:
  void loadMap();

private:
  Entity player;
};
