#pragma once

#include "engine/ecs/entity.h"
#include "engine/input/keyboardInput.h"
#include <SDL_events.h>
class InputController
{
public:
  InputController() = default;
  ~InputController() = default;

  void processKeyboardInput(const sfs::KeyboardInput& input,
                            sfs::Entity& player);
};
