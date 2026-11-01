#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "nlohmann/json.hpp"

namespace sfs
{

/**
 * The scene's weather, as a game sets it each frame (RenderSystem::
 * setWeather(), like the lighting). How cloudy it is stays the lighting's
 * (SceneLighting::cloudCover: the one coverage value); this is the rest.
 *
 * Under a physical sky the clouds are drawn (a layer about 1.5 km up:
 * scattered fair-weather cumulus at low cover, broken cloud in the middle, a
 * grey overcast deck near 1), drifting with `wind`, casting moving shadows on
 * the ground and greying the light as they thicken.
 *
 * @param rain          how hard it's raining now (0: not; 1: a downpour)
 * @param wetness       how wet surfaces are (0: dry; 1: soaked); lags the
 *                      rain, as surfaces wet and dry
 * @param puddles       how full the low spots are (0: dry; 1: full)
 * @param wind          the wind the clouds (and rain) drift on: metres a
 *                      second, in the world's XY
 * @param cloudDarkness how heavy the clouds are (0: white fair-weather
 *                      cloud; 1: dark rain cloud): darker undersides, and
 *                      less of the sun gets through
 */
struct SceneWeather
{
  float rain = 0.0f;
  float wetness = 0.0f;
  float puddles = 0.0f;
  glm::vec2 wind{3.0f, 1.5f};
  float cloudDarkness = 0.0f;

  /** The same, with each value in its range (0..1; the wind as it is). */
  SceneWeather clamped() const;

  nlohmann::json toJson() const;
  /** Missing keys keep `defaults`' values. */
  static SceneWeather fromJson(const nlohmann::json& json,
                               const SceneWeather& defaults);
  static SceneWeather fromJson(const nlohmann::json& json);

  bool operator==(const SceneWeather&) const = default;
};

/**
 * How far the cloud layer's pattern repeats, km (its noise tiles that wide):
 * how far they've drifted is kept within it, so it never loses precision.
 */
inline constexpr float kCloudPeriod = 192.0f;

/**
 * Where the clouds have drifted to (km, in the world's XY; within
 * kCloudPeriod) from `drift`, after `seconds` more on a `wind` of metres a
 * second.
 */
glm::vec2
driftClouds(const glm::vec2& drift, const glm::vec2& wind, float seconds);

} // namespace sfs
