#pragma once

#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/ui/ui.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sfs::ui
{

/**
 * A layer of a game's UI: a script (put it on an entity, as any game object)
 * whose body() describes what it shows, from the game's state, every frame
 * while the scene plays. Layers are drawn over the scene in order, and clicks
 * on them aren't the game's (MouseInput::blocked()).
 *
 * What it drew is kept: a frame whose body() makes the same tree (by its
 * hash, Node::hash()) isn't laid out or drawn again, and one that's changed
 * is only where it's changed. body() itself is still made each frame, unless
 * inputs() says what it shows hasn't changed.
 *
 *   class Hud : public sfs::ui::Layer
 *   {
 *   public:
 *     sfs::ui::View body() override
 *     {
 *       using namespace sfs::ui;
 *       return Text("Coins: " + std::to_string(coins))
 *           .padding().background(Panel()).align(Alignment::TopLeading);
 *     }
 *     int coins = 0;
 *   };
 */
class Layer : public GameObject
{
public:
  /** What it shows now (laid out on the whole screen, placed by its align()).
   */
  virtual View body() = 0;

  /**
   * Optional, for a body() that's costly to make: a hash of everything it's
   * made from (ui::hashOf(...) of the values it shows). While it's the same
   * as last frame's, body() isn't called and last frame's tree is used (its
   * buttons' functions too, so they mustn't hold what's changed). 0, the
   * default: body() is made every frame (and only what's changed in it laid
   * out again).
   */
  virtual uint64_t inputs() { return 0; }

  /** Drawn over layers with lower orders, and clicked first. */
  int order = 0;
  /** Whether it's shown at all. */
  bool visible = true;
};

/** Values mixed into a hash, for Layer::inputs(). */
class Hasher
{
public:
  /** A number, an enum, a bool or a pointer. */
  template <typename T>
    requires std::is_arithmetic_v<T> || std::is_enum_v<T> ||
             std::is_pointer_v<T>
  Hasher& add(T value)
  {
    if constexpr (std::is_floating_point_v<T>)
      return mix(std::hash<T>()(value == T(0) ? T(0) : value));
    else if constexpr (std::is_pointer_v<T>)
      return mix(reinterpret_cast<uintptr_t>(value));
    else
      return mix(static_cast<uint64_t>(value));
  }
  Hasher& add(std::string_view text)
  {
    return mix(std::hash<std::string_view>()(text)).mix(text.size());
  }
  Hasher& add(const std::string& text) { return add(std::string_view(text)); }
  Hasher& add(const char* text) { return add(std::string_view(text)); }
  template <typename T>
  Hasher& add(const std::optional<T>& value)
  {
    mix(value.has_value());
    return value ? add(*value) : *this;
  }
  template <typename T>
  Hasher& add(const std::vector<T>& values)
  {
    mix(values.size());
    for (const T& value : values)
      add(value);
    return *this;
  }
  template <typename A, typename B>
  Hasher& add(const std::pair<A, B>& pair)
  {
    return add(pair.first).add(pair.second);
  }

  uint64_t value() const { return m_hash == 0 ? 1 : m_hash; }

private:
  Hasher& mix(uint64_t value)
  {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    value ^= value >> 31;
    m_hash ^= value + 0x9E3779B97F4A7C15ull + (m_hash << 6) + (m_hash >> 2);
    return *this;
  }

  uint64_t m_hash = 0x484Au;
};

/**
 * A hash of `values` (numbers, enums, strings, and optionals, vectors and
 * pairs of them), never 0: what a Layer::inputs() returns.
 */
template <typename... Values>
uint64_t hashOf(const Values&... values)
{
  Hasher hasher;
  (hasher.add(values), ...);
  return hasher.value();
}

} // namespace sfs::ui
