#include "engine/core/rendering/3d/material.h"

namespace sfs
{

namespace
{
template <typename T>
void read(const nlohmann::json& json, const char* key, T& out)
{
  if (const auto it = json.find(key); it != json.end())
  {
    if constexpr (std::is_same_v<T, std::string>)
    {
      if (it->is_string())
        out = it->get<std::string>();
    }
    else if (it->is_number())
      out = it->get<T>();
  }
}
} // namespace

const char* alphaModeName(AlphaMode mode)
{
  switch (mode)
  {
  case AlphaMode::Mask:
    return "mask";
  case AlphaMode::Blend:
    return "blend";
  case AlphaMode::Opaque:
    break;
  }
  return "opaque";
}

std::optional<AlphaMode> alphaModeFromName(std::string_view name)
{
  for (const AlphaMode mode : {AlphaMode::Opaque, AlphaMode::Mask, AlphaMode::Blend})
    if (name == alphaModeName(mode))
      return mode;
  return std::nullopt;
}

nlohmann::json MaterialAsset::toJson() const
{
  return {
      {"formatVersion", kFormatVersion},
      {"baseColor", {baseColor.r, baseColor.g, baseColor.b, baseColor.a}},
      {"baseTexture", baseTexture},
      {"metallicRoughnessTexture", metallicRoughnessTexture},
      {"tiling", {tiling.x, tiling.y}},
      {"offset", {offset.x, offset.y}},
      {"metallic", metallic},
      {"roughness", roughness},
      {"alphaMode", alphaModeName(alphaMode)},
      {"alphaCutoff", alphaCutoff},
  };
}

std::optional<MaterialAsset> MaterialAsset::fromJson(const nlohmann::json& json)
{
  if (!json.is_object())
    return std::nullopt;

  MaterialAsset material;
  if (const auto it = json.find("baseColor"); it != json.end() && it->is_array() && it->size() == 4)
    for (int i = 0; i < 4; ++i)
      if ((*it)[i].is_number())
        material.baseColor[i] = (*it)[i].get<float>();
  const auto readVec2 = [&json](const char* key, glm::vec2& out)
  {
    if (const auto it = json.find(key); it != json.end() && it->is_array() && it->size() == 2)
      for (int i = 0; i < 2; ++i)
        if ((*it)[i].is_number())
          out[i] = (*it)[i].get<float>();
  };
  readVec2("tiling", material.tiling);
  readVec2("offset", material.offset);
  read(json, "baseTexture", material.baseTexture);
  read(json, "metallicRoughnessTexture", material.metallicRoughnessTexture);
  read(json, "metallic", material.metallic);
  read(json, "roughness", material.roughness);
  read(json, "alphaCutoff", material.alphaCutoff);
  if (const auto it = json.find("alphaMode"); it != json.end() && it->is_string())
    material.alphaMode = alphaModeFromName(it->get<std::string>()).value_or(AlphaMode::Opaque);
  return material;
}

} // namespace sfs
