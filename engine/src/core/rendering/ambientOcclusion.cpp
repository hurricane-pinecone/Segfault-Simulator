#include "engine/core/rendering/ambientOcclusion.h"

#include <algorithm>

namespace sfs
{

nlohmann::json AmbientOcclusionSettings::toJson() const
{
  return {{"enabled", enabled},
          {"radius", radius},
          {"intensity", intensity},
          {"samples", samples}};
}

AmbientOcclusionSettings
AmbientOcclusionSettings::fromJson(const nlohmann::json& json,
                                   const AmbientOcclusionSettings& defaults)
{
  AmbientOcclusionSettings out = defaults;
  if (!json.is_object())
    return out;
  if (const auto it = json.find("enabled");
      it != json.end() && it->is_boolean())
    out.enabled = it->get<bool>();
  if (const auto it = json.find("radius"); it != json.end() && it->is_number())
    out.radius = it->get<float>();
  if (const auto it = json.find("intensity");
      it != json.end() && it->is_number())
    out.intensity = it->get<float>();
  if (const auto it = json.find("samples"); it != json.end() && it->is_number())
    out.samples = static_cast<uint32_t>(
        std::clamp(it->get<double>(),
                   static_cast<double>(kMinOcclusionSamples),
                   static_cast<double>(kMaxOcclusionSamples)));
  return out;
}

AmbientOcclusionSettings
AmbientOcclusionSettings::fromJson(const nlohmann::json& json)
{
  return fromJson(json, AmbientOcclusionSettings());
}

} // namespace sfs
