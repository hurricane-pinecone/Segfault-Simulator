#include "../../testHarness.h"

#include <engine/runtime/input/keyboardInput.h>

using namespace sfs;

// Key-down events, without polling (update() needs SDL running).
int main()
{
  TEST("a key-down event should count as pressed even if released before "
       "polling")
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

  TEST("keys should be found by their names, ignoring case")
  {
    CHECK(keyFromName("Space") == Key::Space);
    CHECK(keyFromName("up") == Key::Up);
    CHECK(keyFromName("F8") == Key::F8);
    CHECK(keyFromName("Num1") == Key::Num1);
    CHECK(keyFromName("W") == Key::W);
    CHECK(!keyFromName("Spacebar").has_value());
    CHECK(!keyFromName("").has_value());
  }

  TEST("a withheld key should read as untouched")
  {
    KeyboardInput keyboard;
    keyboard.beginFrame();
    keyboard.pressKey(SDL_SCANCODE_SPACE);
    keyboard.pressKey(SDL_SCANCODE_A);
    KeyboardInput copy = keyboard;
    copy.withhold(Key::Space);
    CHECK(!copy.keyPressed(Key::Space));
    CHECK(!copy.keyHeld(Key::Space));
    CHECK(!copy.keyReleased(Key::Space));
    CHECK(copy.keyPressed(Key::A));
    CHECK(keyboard.keyPressed(Key::Space)); // the original keeps it
  }

  return testing::report("keyboardInputTests");
}
