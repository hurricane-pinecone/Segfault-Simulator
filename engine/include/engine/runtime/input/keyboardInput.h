#pragma once

#include <SDL_scancode.h>
#include <array>
#include <optional>
#include <string_view>

namespace sfs
{

enum class Key : int
{
  Unknown = SDL_SCANCODE_UNKNOWN,

  A = SDL_SCANCODE_A,
  B = SDL_SCANCODE_B,
  C = SDL_SCANCODE_C,
  D = SDL_SCANCODE_D,
  E = SDL_SCANCODE_E,
  F = SDL_SCANCODE_F,
  G = SDL_SCANCODE_G,
  H = SDL_SCANCODE_H,
  I = SDL_SCANCODE_I,
  J = SDL_SCANCODE_J,
  K = SDL_SCANCODE_K,
  L = SDL_SCANCODE_L,
  M = SDL_SCANCODE_M,
  N = SDL_SCANCODE_N,
  O = SDL_SCANCODE_O,
  P = SDL_SCANCODE_P,
  Q = SDL_SCANCODE_Q,
  R = SDL_SCANCODE_R,
  S = SDL_SCANCODE_S,
  T = SDL_SCANCODE_T,
  U = SDL_SCANCODE_U,
  V = SDL_SCANCODE_V,
  W = SDL_SCANCODE_W,
  X = SDL_SCANCODE_X,
  Y = SDL_SCANCODE_Y,
  Z = SDL_SCANCODE_Z,

  Num1 = SDL_SCANCODE_1,
  Num2 = SDL_SCANCODE_2,
  Num3 = SDL_SCANCODE_3,
  Num4 = SDL_SCANCODE_4,
  Num5 = SDL_SCANCODE_5,
  Num6 = SDL_SCANCODE_6,
  Num7 = SDL_SCANCODE_7,
  Num8 = SDL_SCANCODE_8,
  Num9 = SDL_SCANCODE_9,
  Num0 = SDL_SCANCODE_0,

  Space = SDL_SCANCODE_SPACE,
  Enter = SDL_SCANCODE_RETURN,
  Escape = SDL_SCANCODE_ESCAPE,
  Tab = SDL_SCANCODE_TAB,
  Backspace = SDL_SCANCODE_BACKSPACE,
  Delete = SDL_SCANCODE_DELETE,
  Home = SDL_SCANCODE_HOME,
  End = SDL_SCANCODE_END,
  PageUp = SDL_SCANCODE_PAGEUP,
  PageDown = SDL_SCANCODE_PAGEDOWN,

  // By where they are on a US keyboard (the key, whatever it types).
  Minus = SDL_SCANCODE_MINUS,
  Equals = SDL_SCANCODE_EQUALS,
  LeftBracket = SDL_SCANCODE_LEFTBRACKET,
  RightBracket = SDL_SCANCODE_RIGHTBRACKET,
  Backslash = SDL_SCANCODE_BACKSLASH,
  Semicolon = SDL_SCANCODE_SEMICOLON,
  Apostrophe = SDL_SCANCODE_APOSTROPHE,
  Grave = SDL_SCANCODE_GRAVE,
  Comma = SDL_SCANCODE_COMMA,
  Period = SDL_SCANCODE_PERIOD,
  Slash = SDL_SCANCODE_SLASH,

  LeftShift = SDL_SCANCODE_LSHIFT,
  RightShift = SDL_SCANCODE_RSHIFT,
  LeftCtrl = SDL_SCANCODE_LCTRL,
  RightCtrl = SDL_SCANCODE_RCTRL,
  LeftAlt = SDL_SCANCODE_LALT,
  RightAlt = SDL_SCANCODE_RALT,

  F1 = SDL_SCANCODE_F1,
  F2 = SDL_SCANCODE_F2,
  F3 = SDL_SCANCODE_F3,
  F4 = SDL_SCANCODE_F4,
  F5 = SDL_SCANCODE_F5,
  F6 = SDL_SCANCODE_F6,
  F7 = SDL_SCANCODE_F7,
  F8 = SDL_SCANCODE_F8,
  F9 = SDL_SCANCODE_F9,
  F10 = SDL_SCANCODE_F10,
  F11 = SDL_SCANCODE_F11,
  F12 = SDL_SCANCODE_F12,

  Up = SDL_SCANCODE_UP,
  Down = SDL_SCANCODE_DOWN,
  Left = SDL_SCANCODE_LEFT,
  Right = SDL_SCANCODE_RIGHT,
};

/**
 * The key with a name, as the Key enumerator is spelled ("Space", "Up", "F8",
 * "A", "Num1"), ignoring case: for keys named in data, such as an
 * InputContextComponent's.
 * @param name the key's name
 * @return the key, or nothing for a name no key has
 */
std::optional<Key> keyFromName(std::string_view name);

class KeyboardInput
{
public:
  /** Poll the keyboard state (once per frame, after events are processed). */
  void update();

  /** Start a frame: forget last frame's key-down events. */
  void beginFrame();

  /**
   * A key-down event this frame. keyPressed() reports it even if the key was
   * released again before update() polled (a tap shorter than a frame).
   */
  void pressKey(SDL_Scancode key);

  bool keyHeld(Key key) const;
  bool keyPressed(Key key) const;
  bool keyReleased(Key key) const;

  /**
   * Makes a key read as untouched from now on in this copy of the keyboard:
   * not held, pressed or released. A scene withholds the keys an input
   * context claims from the objects it outranks.
   * @param key the key to withhold
   */
  void withhold(Key key);

private:
  std::array<bool, SDL_NUM_SCANCODES> currentKeys{};
  std::array<bool, SDL_NUM_SCANCODES> downThisFrame{};
  std::array<bool, SDL_NUM_SCANCODES> previousKeys{};
};

}; // namespace sfs
