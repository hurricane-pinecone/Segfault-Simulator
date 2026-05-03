
#pragma once

#include "InputController/InputController.h"
#include "engine/ecs/entity.h"
#include "engine/input/keyboardInput.h"
#include <SDL_events.h>
#include <engine/game/game.h>

class SampleGame : public sfs::Game
{
public:
  SampleGame() { inputController = InputController(); }
  ~SampleGame() = default;

protected:
  void onSetup() override;
  void onProcessInput(const sfs::KeyboardInput& keyboardInput) override;
  void onDestroy() override;

private:
  void loadMap();

private:
  sfs::Entity player;
  InputController inputController;
};
