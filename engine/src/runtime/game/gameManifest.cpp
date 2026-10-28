#include "engine/runtime/game/gameManifest.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/serialization/sceneSerializer.h"

namespace sfs
{

std::optional<GameManifest>
GameManifest::read(const std::filesystem::path& folder)
{
  const std::filesystem::path file = folder / kFileName;
  const std::optional<nlohmann::json> json =
      SceneSerializer::readJsonFile(file);
  if (!json || !json->is_object())
  {
    LOG_ERROR("No game description at " + file.string());
    return std::nullopt;
  }
  if (json->value("formatVersion", 0) > kFormatVersion)
  {
    LOG_ERROR(file.string() + " was written by a newer engine");
    return std::nullopt;
  }

  GameManifest manifest;
  manifest.name = json->value("name", std::string());
  manifest.moduleName = json->value("moduleName", std::string());
  manifest.module = json->value("module", std::string());
  manifest.startScene = json->value("startScene", std::string());
  manifest.windowWidth = json->value("windowWidth", manifest.windowWidth);
  manifest.windowHeight = json->value("windowHeight", manifest.windowHeight);

  if (manifest.startScene.empty())
  {
    LOG_ERROR(file.string() + " names no start scene");
    return std::nullopt;
  }
  return manifest;
}

bool GameManifest::write(const std::filesystem::path& folder) const
{
  // Forward slashes, so a manifest reads the same on every platform.
  const nlohmann::json json = {{"formatVersion", kFormatVersion},
                               {"name", name},
                               {"moduleName", moduleName},
                               {"module", module.generic_string()},
                               {"startScene", startScene.generic_string()},
                               {"windowWidth", windowWidth},
                               {"windowHeight", windowHeight}};
  return SceneSerializer::writeJsonFile(json, folder / kFileName);
}

} // namespace sfs
