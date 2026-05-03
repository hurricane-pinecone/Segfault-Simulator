#include <SDL_events.h>
#include <engine/input/keyboardInput.h>

namespace sfs
{

void KeyboardInput::update()
{
  previousKeys = currentKeys;

  SDL_PumpEvents();

  int keyCount = 0;
  const Uint8* keys = SDL_GetKeyboardState(&keyCount);

  for (int i = 0; i < keyCount; ++i)
  {
    currentKeys[i] = keys[i] != 0;
  }
}

bool KeyboardInput::keyHeld(Key key) const
{
  return currentKeys[static_cast<int>(key)];
}

bool KeyboardInput::keyPressed(Key key) const
{
  int k = static_cast<int>(key);
  return currentKeys[k] && !previousKeys[k];
}

bool KeyboardInput::keyReleased(Key key) const
{
  int k = static_cast<int>(key);
  return !currentKeys[k] && previousKeys[k];
}

} // namespace sfs
