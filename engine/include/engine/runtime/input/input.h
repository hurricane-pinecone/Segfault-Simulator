#pragma once

#include "engine/runtime/input/keyboardInput.h"
#include "engine/runtime/input/mouseInput.h"

namespace sfs
{

class Input
{
public:
  const KeyboardInput& keyboard() const;
  KeyboardInput& keyboard();
  MouseInput& mouse();
  const MouseInput& mouse() const;
  void update();
  // const GamepadInput& gamepad() const;

private:
  KeyboardInput keyboardInput;
  MouseInput mouseInput;
  // GamepadInput gamepadInput;
};

} // namespace sfs
