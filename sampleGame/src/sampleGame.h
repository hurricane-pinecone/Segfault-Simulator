
#pragma once

#include "InputController/InputController.h"
#include "engine/ecs/entity.h"
#include <SDL_events.h>
#include <engine/game/game.h>

class SampleGame : public sfs::Game
{
public:
  SampleGame() { inputController = InputController(); }
  ~SampleGame() = default;

protected:
  void onSetup() override;
  void onProcessInput(const sfs::Input& input) override;
  void onDestroy() override;

private:
  void loadMap();

private:
  sfs::Entity player;
  sfs::Entity camera;
  InputController inputController;
};
