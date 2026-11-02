#include "engine/core/rendering/rain.h"

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

float unit(float value)
{
  // (NaN to 0.)
  return value > 0.0f ? std::min(value, 1.0f) : 0.0f;
}

// At least 0 (NaN to 0), at most `most`.
float upTo(float value, float most)
{
  return value > 0.0f ? std::min(value, most) : 0.0f;
}

} // namespace

nlohmann::json RainSettings::toJson() const
{
  return {{"enabled", enabled},
          {"density", density},
          {"length", length},
          {"width", width},
          {"brightness", brightness},
          {"splashes", splashes},
          {"ripples", ripples},
          {"veil", veil},
          {"drips", drips}};
}

RainSettings RainSettings::fromJson(const nlohmann::json& json,
                                    const RainSettings& defaults)
{
  RainSettings out = defaults;
  if (!json.is_object())
    return out;
  if (const auto it = json.find("enabled");
      it != json.end() && it->is_boolean())
    out.enabled = it->get<bool>();
  read(json, "density", out.density);
  read(json, "length", out.length);
  read(json, "width", out.width);
  read(json, "brightness", out.brightness);
  read(json, "splashes", out.splashes);
  read(json, "ripples", out.ripples);
  read(json, "veil", out.veil);
  read(json, "drips", out.drips);
  return out;
}

RainSettings RainSettings::fromJson(const nlohmann::json& json)
{
  return fromJson(json, RainSettings());
}

uint32_t rainStreaks(const RainSettings& settings, float rain)
{
  if (!settings.enabled)
    return 0;
  // Up to twice the usual, so a style can't ask for millions. Many more as
  // it rains harder: a drizzle (0.3) has an eighth of a downpour's.
  const float share =
      std::pow(unit(rain), 1.75f) * upTo(settings.density, 2.0f);
  return static_cast<uint32_t>(std::round(share * kRainStreaks));
}

uint32_t rainSplashes(const RainSettings& settings, float rain)
{
  if (!settings.enabled)
    return 0;
  const float share =
      std::pow(unit(rain), 1.5f) * upTo(settings.splashes, 2.0f);
  return static_cast<uint32_t>(std::round(share * kRainSplashes));
}

float rainVeilDensity(const RainSettings& settings, float rain)
{
  if (!settings.enabled)
    return 0.0f;
  // Light rain barely veils the distance; a downpour (1) hides half of what's
  // 150 m away: 1 - exp(-150 d) = 0.5.
  const float hard = unit(rain);
  constexpr float kDownpour = 0.00462f;
  return kDownpour * hard * std::sqrt(hard) * upTo(settings.veil, 4.0f);
}

RainDrips rainDrips(const RainSettings& settings, float rain, float wetness)
{
  RainDrips out;
  if (!settings.enabled)
    return out;
  // Heavy rain drips off every edge; light rain soaks in where it falls.
  // After it stops, soaked roofs still drip for a while, a quarter as much
  // and slowly, until they've dried off.
  const auto ease = [](float from, float to, float x)
  {
    const float t = std::clamp((x - from) / (to - from), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  };
  const float heavy = ease(0.4f, 1.0f, unit(rain));
  const float soaked = 0.25f * ease(0.5f, 1.0f, unit(wetness));
  const float share = std::max(heavy, soaked) * upTo(settings.drips, 2.0f);
  out.count = static_cast<uint32_t>(std::round(share * kRainDrips));
  if (out.count == 0)
    return out;
  out.rate = 0.3f + 0.7f * heavy;
  out.trickle = ease(0.7f, 1.0f, unit(rain));
  // The drip line soaks as the ground around it does, wetter while it pours.
  out.line = unit(wetness) * std::max(heavy, 0.5f) *
             std::min(upTo(settings.drips, 2.0f), 1.0f);
  return out;
}

RainMap rainMapFor(const glm::vec3& camera, const glm::vec3& forward)
{
  // Ahead of the camera, where most of what it sees is, the camera still
  // well inside.
  RainMap map;
  map.halfSize = kRainMapHalfSize;
  glm::vec2 ahead(forward.x, forward.y);
  const float along = std::sqrt(glm::dot(ahead, ahead));
  glm::vec2 center(camera.x, camera.y);
  if (along > 1e-3f && std::isfinite(along))
    center += ahead / along * (0.6f * kRainMapHalfSize);
  const float texel = 2.0f * kRainMapHalfSize / kRainMapResolution;
  map.center = glm::round(center / texel) * texel;
  if (!std::isfinite(map.center.x) || !std::isfinite(map.center.y))
    map.center = glm::vec2(0.0f);

  // From well above the camera to well below it (in whole metres).
  map.top = std::isfinite(camera.z) ? std::round(camera.z) + 200.0f : 200.0f;
  map.depthRange = 600.0f;

  // x, y across the square (-1..1), z down from the top (0..1).
  glm::mat4& m = map.viewProjection;
  m = glm::mat4(0.0f);
  m[0][0] = 1.0f / map.halfSize;
  m[1][1] = 1.0f / map.halfSize;
  m[2][2] = -1.0f / map.depthRange;
  m[3][0] = -map.center.x / map.halfSize;
  m[3][1] = -map.center.y / map.halfSize;
  m[3][2] = map.top / map.depthRange;
  m[3][3] = 1.0f;
  return map;
}

void RainDrift::advance(const glm::vec2& target, float seconds)
{
  if (!std::isfinite(target.x) || !std::isfinite(target.y) ||
      !std::isfinite(seconds) || seconds < 0.0f)
    return;
  if (!started)
  {
    wind = target;
    started = true;
  }
  // Eased over about a second (exactly, however the frames fall).
  wind += (target - wind) * (1.0f - std::exp(-seconds / 1.2f));
  drift += wind * (0.8f * seconds);
  for (int axis = 0; axis < 2; ++axis)
  {
    drift[axis] = std::fmod(drift[axis], kRainDriftPeriod);
    if (drift[axis] < 0.0f)
      drift[axis] += kRainDriftPeriod;
    if (!std::isfinite(drift[axis]) || drift[axis] >= kRainDriftPeriod)
      drift[axis] = 0.0f;
  }
}

} // namespace sfs
