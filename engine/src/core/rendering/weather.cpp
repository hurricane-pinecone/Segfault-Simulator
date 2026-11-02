#include "engine/core/rendering/weather.h"

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

} // namespace

SceneWeather SceneWeather::clamped() const
{
  SceneWeather out = *this;
  out.rain = unit(rain);
  out.wetness = unit(wetness);
  out.puddles = unit(puddles);
  out.cloudDarkness = unit(cloudDarkness);
  if (!std::isfinite(out.wind.x) || !std::isfinite(out.wind.y))
    out.wind = glm::vec2(0.0f);
  return out;
}

nlohmann::json SceneWeather::toJson() const
{
  return {{"rain", rain},
          {"wetness", wetness},
          {"puddles", puddles},
          {"wind", {wind.x, wind.y}},
          {"cloudDarkness", cloudDarkness}};
}

SceneWeather SceneWeather::fromJson(const nlohmann::json& json,
                                    const SceneWeather& defaults)
{
  SceneWeather out = defaults;
  if (!json.is_object())
    return out;
  read(json, "rain", out.rain);
  read(json, "wetness", out.wetness);
  read(json, "puddles", out.puddles);
  read(json, "cloudDarkness", out.cloudDarkness);
  if (const auto it = json.find("wind");
      it != json.end() && it->is_array() && it->size() == 2 &&
      (*it)[0].is_number() && (*it)[1].is_number())
    out.wind = {(*it)[0].get<float>(), (*it)[1].get<float>()};
  return out;
}

SceneWeather SceneWeather::fromJson(const nlohmann::json& json)
{
  return fromJson(json, SceneWeather());
}

glm::vec2
driftClouds(const glm::vec2& drift, const glm::vec2& wind, float seconds)
{
  glm::vec2 out = drift;
  if (std::isfinite(wind.x) && std::isfinite(wind.y) && std::isfinite(seconds))
    out += wind * (0.001f * seconds);
  for (int axis = 0; axis < 2; ++axis)
  {
    out[axis] = std::fmod(out[axis], kCloudPeriod);
    if (out[axis] < 0.0f)
      out[axis] += kCloudPeriod;
    if (!std::isfinite(out[axis]) || out[axis] >= kCloudPeriod)
      out[axis] = 0.0f;
  }
  return out;
}

} // namespace sfs
