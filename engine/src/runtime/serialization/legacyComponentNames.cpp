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
  for (auto& [key, value] : file.items())
    upgradeComponentNames(value);
}

} // namespace sfs
