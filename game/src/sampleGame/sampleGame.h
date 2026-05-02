#pragma once

#include <engine/game/game.h>

class SampleGame : public Game
{
protected:
  void onSetup() override;
  void onDestroy() override;
};
