#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace sfs
{

/**
 * UI-agnostic description of a single setting a render module exposes. A debug
 * panel, editor, or serializer reads the descriptor's type and range and binds
 * to the live value through the get/set accessors -- no UI toolkit is
 * referenced here, so the same descriptors drive ImGui today and an editor or
 * serialization layer later.
 */
struct ModuleSetting
{
  enum class Type
  {
    Float,  // Continuous value in [min, max]; uses getFloat/setFloat.
    Bool,   // Toggle; uses getBool/setBool.
    Enum,   // One of @ref options (by index); uses getEnum/setEnum.
    Action, // One-shot command (e.g. a button); uses onInvoke.
    Text,   // Read-only label; uses getText.
  };

  /** Display label / identifier for the setting. */
  std::string label;

  /** Which accessor group is valid. */
  Type type = Type::Float;

  /** When false the setting is exposed but not applicable in the current mode;
   * a consumer should show it read-only/greyed. */
  bool enabled = true;

  // --- Float ---
  float min = 0.0f;
  float max = 1.0f;
  std::function<float()> getFloat;
  std::function<void(float)> setFloat;

  // --- Bool ---
  std::function<bool()> getBool;
  std::function<void(bool)> setBool;

  // --- Enum ---
  std::vector<std::string> options;
  std::function<int()> getEnum;
  std::function<void(int)> setEnum;

  // --- Action ---
  std::function<void()> onInvoke;

  // --- Text ---
  std::function<std::string()> getText;
};

/** Factory helpers for the common @ref ModuleSetting kinds. */
namespace settings
{

/** A float slider over [min, max]. */
inline ModuleSetting floatRange(std::string label,
                                float min,
                                float max,
                                std::function<float()> get,
                                std::function<void(float)> set)
{
  ModuleSetting s;
  s.label = std::move(label);
  s.type = ModuleSetting::Type::Float;
  s.min = min;
  s.max = max;
  s.getFloat = std::move(get);
  s.setFloat = std::move(set);
  return s;
}

/** A boolean toggle. */
inline ModuleSetting boolean(std::string label,
                             std::function<bool()> get,
                             std::function<void(bool)> set)
{
  ModuleSetting s;
  s.label = std::move(label);
  s.type = ModuleSetting::Type::Bool;
  s.getBool = std::move(get);
  s.setBool = std::move(set);
  return s;
}

/** A choice among @p options, selected by index. */
inline ModuleSetting enumChoice(std::string label,
                                std::vector<std::string> options,
                                std::function<int()> get,
                                std::function<void(int)> set)
{
  ModuleSetting s;
  s.label = std::move(label);
  s.type = ModuleSetting::Type::Enum;
  s.options = std::move(options);
  s.getEnum = std::move(get);
  s.setEnum = std::move(set);
  return s;
}

/** A one-shot command (rendered as a button). */
inline ModuleSetting action(std::string label, std::function<void()> invoke)
{
  ModuleSetting s;
  s.label = std::move(label);
  s.type = ModuleSetting::Type::Action;
  s.onInvoke = std::move(invoke);
  return s;
}

/** A read-only text readout. */
inline ModuleSetting text(std::string label, std::function<std::string()> get)
{
  ModuleSetting s;
  s.label = std::move(label);
  s.type = ModuleSetting::Type::Text;
  s.getText = std::move(get);
  return s;
}

} // namespace settings
} // namespace sfs
