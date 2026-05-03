#pragma once

#include "engine/input/mouseInput.h"
#include <engine/input/keyboardInput.h>

namespace sfs
{

class Input
{
public:
  void update();

  const KeyboardInput& keyboard() const;
  MouseInput& mouse();
  const MouseInput& mouse() const;
  // const GamepadInput& gamepad() const;

private:
  KeyboardInput keyboardInput;
  MouseInput mouseInput;
  // GamepadInput gamepadInput;
};

} // namespace sfs
