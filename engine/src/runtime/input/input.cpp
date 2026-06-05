#include "engine/runtime/input/keyboardInput.h"
#include "engine/runtime/input/mouseInput.h"
#include <engine/runtime/input/input.h>

namespace sfs
{

void Input::update() { keyboardInput.update(); }

const KeyboardInput& Input::keyboard() const { return keyboardInput; }
MouseInput& Input::mouse() { return mouseInput; }
const MouseInput& Input::mouse() const { return mouseInput; }

} // namespace sfs
