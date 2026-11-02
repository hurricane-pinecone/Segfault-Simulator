#include "engine/core/rendering/fog.h"

#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>

namespace sfs
{

namespace
{

void read(const nlohmann::json& json, const char* key, float& out)
{
  if (const auto it = json.find(key); it != json.end() && it->is_number())
    out = it->get<float>();
}

} // namespace

nlohmann::json FogSettings::toJson() const
{
  return {{"enabled", enabled},
          {"density", density},
          {"heightFalloff", heightFalloff},
          {"baseHeight", baseHeight},
          {"startDistance", startDistance},
          {"maxOpacity", maxOpacity},
          {"sunGlow", sunGlow}};
}

FogSettings FogSettings::fromJson(const nlohmann::json& json,
                                  const FogSettings& defaults)
{
  FogSettings out = defaults;
  if (!json.is_object())
    return out;
  if (const auto it = json.find("enabled");
      it != json.end() && it->is_boolean())
    out.enabled = it->get<bool>();
  read(json, "density", out.density);
  read(json, "heightFalloff", out.heightFalloff);
  read(json, "baseHeight", out.baseHeight);
  read(json, "startDistance", out.startDistance);
  read(json, "maxOpacity", out.maxOpacity);
  read(json, "sunGlow", out.sunGlow);
  return out;
}

FogSettings FogSettings::fromJson(const nlohmann::json& json)
{
  return fromJson(json, FogSettings());
}

float fogOpacity(const FogSettings& fog,
                 const glm::vec3& camera,
                 const glm::vec3& point)
{
  if (!fog.enabled)
    return 0.0f;
  const glm::vec3 away = point - camera;
  const float length = glm::length(away);
  const float start = std::max(fog.startDistance, 0.0f);
  if (length <= start || length <= 0.0f)
    return 0.0f;

  // Through the fog from where it starts to the point: density
  // d * exp(-k (z - base)) along a line whose height changes by `rise`, which
  // integrates to (exp(-k (z0 - base)) - exp(-k (z1 - base))) / (k rise)
  // times its length (or, level, the density there times the length).
  const float travelled = length - start;
  const float startHeight = camera.z + away.z * (start / length);
  const float rise = away.z * (travelled / length);
  const float k = std::max(fog.heightFalloff, 0.0f);
  const float near =
      std::exp(std::min(-k * (startHeight - fog.baseHeight), 80.0f));
  const float far =
      std::exp(std::min(-k * (startHeight + rise - fog.baseHeight), 80.0f));
  const float slope = k * rise;
  const float mean =
      std::abs(slope) > 1e-3f ? (near - far) / slope : 0.5f * (near + far);
  const float depth = std::max(fog.density, 0.0f) * travelled * mean;
  return std::min(
      1.0f - std::exp(-depth), std::clamp(fog.maxOpacity, 0.0f, 1.0f));
}

float fogPhase(float cosAngle, float sharpness)
{
  const float g = std::clamp(sharpness, -0.99f, 0.99f);
  const float denominator =
      1.0f + g * g - 2.0f * g * std::clamp(cosAngle, -1.0f, 1.0f);
  return (1.0f - g * g) /
         (4.0f * 3.14159265f * denominator * std::sqrt(denominator));
}

} // namespace sfs
