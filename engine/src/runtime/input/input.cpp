#include "engine/runtime/input/input.h"

#include "engine/runtime/input/keyboardInput.h"
#include "engine/runtime/input/mouseInput.h"

namespace sfs
{

const KeyboardInput& Input::keyboard() const { return keyboardInput; }
KeyboardInput& Input::keyboard() { return keyboardInput; }
MouseInput& Input::mouse() { return mouseInput; }
const MouseInput& Input::mouse() const { return mouseInput; }

void Input::update() { keyboardInput.update(); }

} // namespace sfs
