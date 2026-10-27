#include "../../testHarness.h"

#include <engine/runtime/input/keyboardInput.h>

using namespace sfs;

// Key-down events, without polling (update() needs SDL running).
int main()
{
  TEST("a key-down event should count as pressed even if released before polling")
  {
    KeyboardInput keyboard;
    keyboard.beginFrame();
    keyboard.pressKey(SDL_SCANCODE_SPACE);
    CHECK(keyboard.keyPressed(Key::Space));
    CHECK(!keyboard.keyHeld(Key::Space)); // the state never saw it down
    CHECK(!keyboard.keyPressed(Key::A));
  }

  TEST("a press should last only its frame")
  {
    KeyboardInput keyboard;
    keyboard.beginFrame();
    keyboard.pressKey(SDL_SCANCODE_RIGHT);
    keyboard.beginFrame();
    CHECK(!keyboard.keyPressed(Key::Right));
  }

  TEST("out-of-range scancodes should be ignored")
  {
    KeyboardInput keyboard;
    keyboard.beginFrame();
    keyboard.pressKey(static_cast<SDL_Scancode>(-1));
    keyboard.pressKey(SDL_NUM_SCANCODES);
    CHECK(!keyboard.keyPressed(Key::Space));
  }

  return testing::report("keyboardInputTests");
}
