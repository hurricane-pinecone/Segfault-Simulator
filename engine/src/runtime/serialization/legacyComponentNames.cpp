#include "engine/runtime/serialization/legacyComponentNames.h"

#include <string>
#include <string_view>
#include <utility>

namespace sfs
{

namespace
{
// Retired component name -> current name.
constexpr std::pair<std::string_view, std::string_view> kRenamed[] = {
    {"Transform3DComponent", "TransformComponent"},
    {"Camera3DComponent", "CameraComponent"},
    {"PointLight3DComponent", "PointLightComponent"},
};

// Components since folded into SpriteComponent: their values move onto the
// entity's sprite (an entity without one loses them; they did nothing).
void foldIntoSprite(nlohmann::json& components)
{
  const auto tint = components.find("SpriteTint");
  if (tint != components.end())
  {
    const auto sprite = components.find("SpriteComponent");
    if (sprite != components.end() && sprite->is_object() &&
        !sprite->contains("tint") && tint->is_object())
    {
      nlohmann::json color = tint->value("color", nlohmann::json::array());
      if (!color.is_array() || color.size() != 3)
        color = {1.0, 1.0, 1.0};
      color.push_back(tint->value("alpha", 1.0));
      (*sprite)["tint"] = std::move(color);
    }
    components.erase(tint);
  }

  const auto shadow = components.find("ShadowCasterComponent");
  if (shadow != components.end())
  {
    const auto sprite = components.find("SpriteComponent");
    if (sprite != components.end() && sprite->is_object())
      (*sprite)["castsShadow"] = true;
    components.erase(shadow);
  }
}
} // namespace

void upgradeComponentNames(nlohmann::json& file)
{
  if (file.is_array())
  {
    for (nlohmann::json& item : file)
      upgradeComponentNames(item);
    return;
  }
  if (!file.is_object())
    return;

  for (const auto& [from, to] : kRenamed)
  {
    const std::string oldName(from);
    const auto it = file.find(oldName);
    if (it == file.end())
      continue;
    // A file holding both keeps the current one.
    const std::string newName(to);
    if (!file.contains(newName))
      file[newName] = std::move(*it);
    file.erase(oldName);
  }
  foldIntoSprite(file);
  for (auto& [key, value] : file.items())
    upgradeComponentNames(value);
}

} // namespace sfs
