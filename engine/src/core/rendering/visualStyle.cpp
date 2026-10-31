#include "engine/core/rendering/visualStyle.h"

#include "engine/core/rendering/sunShadow.h"

#include <algorithm>

namespace sfs
{

namespace
{

template <typename T>
void read(const nlohmann::json& json, const char* key, T& out)
{
  if (const auto it = json.find(key); it != json.end())
  {
    if constexpr (std::is_same_v<T, bool>)
    {
      if (it->is_boolean())
        out = it->get<bool>();
    }
    else if (it->is_number())
      out = it->get<T>();
  }
}

void readVec3(const nlohmann::json& json, const char* key, glm::vec3& out)
{
  if (const auto it = json.find(key);
      it != json.end() && it->is_array() && it->size() == 3)
    for (int i = 0; i < 3; ++i)
      if ((*it)[i].is_number())
        out[i] = (*it)[i].get<float>();
}

nlohmann::json vec3Json(const glm::vec3& v) { return {v.x, v.y, v.z}; }

} // namespace

nlohmann::json bloomToJson(const BloomSettings& bloom)
{
  return {{"enabled", bloom.enabled},
          {"intensity", bloom.intensity},
          {"threshold", bloom.threshold},
          {"softness", bloom.softness},
          {"radius", bloom.radius}};
}

BloomSettings bloomFromJson(const nlohmann::json& json,
                            const BloomSettings& defaults)
{
  BloomSettings out = defaults;
  if (!json.is_object())
    return out;
  read(json, "enabled", out.enabled);
  read(json, "intensity", out.intensity);
  read(json, "threshold", out.threshold);
  read(json, "softness", out.softness);
  read(json, "radius", out.radius);
  return out;
}

nlohmann::json VisualStyle::toJson() const
{
  nlohmann::json out = {
      {"formatVersion", kFormatVersion},
      {"grading", grading.toJson()},
      {"lighting",
       {{"sunIntensity", sunIntensity},
        {"sunTint", vec3Json(sunTint)},
        {"ambientIntensity", ambientIntensity},
        {"ambientTint", vec3Json(ambientTint)},
        {"skyIntensity", skyIntensity},
        {"skyTint", vec3Json(skyTint)}}},
      {"materialVariant", materialVariant},
  };
  if (bloom)
    out["bloom"] = bloomToJson(*bloom);
  if (haze)
    out["haze"] = {{"strength", haze->strength}, {"distance", haze->distance}};
  nlohmann::json shadows = nlohmann::json::object();
  if (shadowSoftness)
    shadows["softness"] = *shadowSoftness;
  if (shadowCascades)
    shadows["cascades"] = *shadowCascades;
  if (shadowCascadeDistance)
    shadows["cascadeDistance"] = *shadowCascadeDistance;
  if (shadowCascadeSplit)
    shadows["cascadeSplit"] = *shadowCascadeSplit;
  if (!shadows.empty())
    out["shadows"] = std::move(shadows);
  if (sky)
    out["sky"] = sky->toJson();
  if (ambientOcclusion)
    out["ambientOcclusion"] = ambientOcclusion->toJson();
  if (contactShadows)
    out["contactShadows"] = contactShadows->toJson();
  if (fog)
    out["fog"] = fog->toJson();
  if (!materials.empty())
    out["materials"] = materials;
  return out;
}

ShadowSettings VisualStyle::shadows(const ShadowSettings& scene) const
{
  ShadowSettings out = scene;
  if (shadowSoftness)
    out.softness = *shadowSoftness;
  if (shadowCascades)
    out.cascades = *shadowCascades;
  if (shadowCascadeDistance)
    out.cascadeDistance = *shadowCascadeDistance;
  if (shadowCascadeSplit)
    out.cascadeSplit = *shadowCascadeSplit;
  return out;
}

std::optional<VisualStyle> VisualStyle::fromJson(const nlohmann::json& json)
{
  if (!json.is_object())
    return std::nullopt;

  VisualStyle style;
  if (const auto it = json.find("grading"); it != json.end())
    style.grading = ColorGrading::fromJson(*it);
  if (const auto it = json.find("lighting");
      it != json.end() && it->is_object())
  {
    read(*it, "sunIntensity", style.sunIntensity);
    readVec3(*it, "sunTint", style.sunTint);
    read(*it, "ambientIntensity", style.ambientIntensity);
    readVec3(*it, "ambientTint", style.ambientTint);
    read(*it, "skyIntensity", style.skyIntensity);
    readVec3(*it, "skyTint", style.skyTint);
  }
  if (const auto it = json.find("bloom"); it != json.end() && it->is_object())
    style.bloom = bloomFromJson(*it);
  if (const auto it = json.find("haze"); it != json.end() && it->is_object())
  {
    HazeSettings haze;
    read(*it, "strength", haze.strength);
    read(*it, "distance", haze.distance);
    style.haze = haze;
  }
  if (const auto it = json.find("shadows"); it != json.end() && it->is_object())
  {
    const auto number = [&](const char* key) -> std::optional<float>
    {
      if (const auto value = it->find(key);
          value != it->end() && value->is_number())
        return value->get<float>();
      return std::nullopt;
    };
    style.shadowSoftness = number("softness");
    if (const std::optional<float> cascades = number("cascades"))
      style.shadowCascades = static_cast<uint32_t>(
          std::clamp(*cascades, 1.0f, static_cast<float>(kMaxShadowCascades)));
    style.shadowCascadeDistance = number("cascadeDistance");
    style.shadowCascadeSplit = number("cascadeSplit");
  }
  if (const auto it = json.find("sky"); it != json.end() && it->is_object())
    style.sky = SkySettings::fromJson(*it);
  if (const auto it = json.find("ambientOcclusion");
      it != json.end() && it->is_object())
    style.ambientOcclusion = AmbientOcclusionSettings::fromJson(*it);
  if (const auto it = json.find("contactShadows");
      it != json.end() && it->is_object())
    style.contactShadows = ContactShadowSettings::fromJson(*it);
  if (const auto it = json.find("fog"); it != json.end() && it->is_object())
    style.fog = FogSettings::fromJson(*it);
  if (const auto it = json.find("materials");
      it != json.end() && it->is_object())
    for (const auto& [name, material] : it->items())
      if (!name.empty() && material.is_string() &&
          !material.get<std::string>().empty())
        style.materials[name] = material.get<std::string>();
  if (const auto it = json.find("materialVariant");
      it != json.end() && it->is_string())
    style.materialVariant = it->get<std::string>();
  return style;
}

} // namespace sfs
