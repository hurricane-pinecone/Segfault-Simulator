#include <SDL_events.h>
#include <engine/runtime/input/keyboardInput.h>

#include <cctype>
#include <utility>

namespace sfs
{

namespace
{
// Every Key by its enumerator's name.
constexpr std::pair<std::string_view, Key> kKeyNames[] = {
    {"A", Key::A},
    {"B", Key::B},
    {"C", Key::C},
    {"D", Key::D},
    {"E", Key::E},
    {"F", Key::F},
    {"G", Key::G},
    {"H", Key::H},
    {"I", Key::I},
    {"J", Key::J},
    {"K", Key::K},
    {"L", Key::L},
    {"M", Key::M},
    {"N", Key::N},
    {"O", Key::O},
    {"P", Key::P},
    {"Q", Key::Q},
    {"R", Key::R},
    {"S", Key::S},
    {"T", Key::T},
    {"U", Key::U},
    {"V", Key::V},
    {"W", Key::W},
    {"X", Key::X},
    {"Y", Key::Y},
    {"Z", Key::Z},
    {"Num1", Key::Num1},
    {"Num2", Key::Num2},
    {"Num3", Key::Num3},
    {"Num4", Key::Num4},
    {"Num5", Key::Num5},
    {"Num6", Key::Num6},
    {"Num7", Key::Num7},
    {"Num8", Key::Num8},
    {"Num9", Key::Num9},
    {"Num0", Key::Num0},
    {"Space", Key::Space},
    {"Enter", Key::Enter},
    {"Escape", Key::Escape},
    {"Tab", Key::Tab},
    {"Backspace", Key::Backspace},
    {"Delete", Key::Delete},
    {"Home", Key::Home},
    {"End", Key::End},
    {"PageUp", Key::PageUp},
    {"PageDown", Key::PageDown},
    {"Minus", Key::Minus},
    {"Equals", Key::Equals},
    {"LeftBracket", Key::LeftBracket},
    {"RightBracket", Key::RightBracket},
    {"Backslash", Key::Backslash},
    {"Semicolon", Key::Semicolon},
    {"Apostrophe", Key::Apostrophe},
    {"Grave", Key::Grave},
    {"Comma", Key::Comma},
    {"Period", Key::Period},
    {"Slash", Key::Slash},
    {"LeftShift", Key::LeftShift},
    {"RightShift", Key::RightShift},
    {"LeftCtrl", Key::LeftCtrl},
    {"RightCtrl", Key::RightCtrl},
    {"LeftAlt", Key::LeftAlt},
    {"RightAlt", Key::RightAlt},
    {"F1", Key::F1},
    {"F2", Key::F2},
    {"F3", Key::F3},
    {"F4", Key::F4},
    {"F5", Key::F5},
    {"F6", Key::F6},
    {"F7", Key::F7},
    {"F8", Key::F8},
    {"F9", Key::F9},
    {"F10", Key::F10},
    {"F11", Key::F11},
    {"F12", Key::F12},
    {"Up", Key::Up},
    {"Down", Key::Down},
    {"Left", Key::Left},
    {"Right", Key::Right},
};

bool sameIgnoringCase(std::string_view a, std::string_view b)
{
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}
} // namespace

std::optional<Key> keyFromName(std::string_view name)
{
  for (const auto& [keyName, key] : kKeyNames)
    if (sameIgnoringCase(keyName, name))
      return key;
  return std::nullopt;
}

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

void KeyboardInput::beginFrame() { downThisFrame.fill(false); }

void KeyboardInput::pressKey(SDL_Scancode key)
{
  if (key >= 0 && key < SDL_NUM_SCANCODES)
    downThisFrame[key] = true;
}

bool KeyboardInput::keyPressed(Key key) const
{
  int k = static_cast<int>(key);
  return (currentKeys[k] && !previousKeys[k]) || downThisFrame[k];
}

bool KeyboardInput::keyReleased(Key key) const
{
  int k = static_cast<int>(key);
  return !currentKeys[k] && previousKeys[k];
}

void KeyboardInput::withhold(Key key)
{
  const int k = static_cast<int>(key);
  if (k < 0 || k >= SDL_NUM_SCANCODES)
    return;
  currentKeys[k] = false;
  previousKeys[k] = false;
  downThisFrame[k] = false;
}

} // namespace sfs
