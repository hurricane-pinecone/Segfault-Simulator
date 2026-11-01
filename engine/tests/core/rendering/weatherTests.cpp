#include "../../testHarness.h"

#include <engine/core/rendering/weather.h>

#include <cmath>
#include <limits>

using namespace sfs;

int main()
{
  TEST("weather should default to dry, and round-trip")
  {
    const SceneWeather defaults;
    CHECK(defaults.rain == 0.0f);
    CHECK(defaults.wetness == 0.0f);
    CHECK(defaults.puddles == 0.0f);
    CHECK(defaults.cloudDarkness == 0.0f);
    CHECK(SceneWeather::fromJson(defaults.toJson()) == defaults);

    SceneWeather wet;
    wet.rain = 0.75f;
    wet.wetness = 0.5f;
    wet.puddles = 0.25f;
    wet.wind = {-4.0f, 2.5f};
    wet.cloudDarkness = 0.6f;
    CHECK(SceneWeather::fromJson(wet.toJson()) == wet);

    // Missing (or mistyped) keys keep the defaults given.
    nlohmann::json partial = {{"rain", 0.5f}, {"wind", "east"}};
    const SceneWeather read = SceneWeather::fromJson(partial, wet);
    CHECK(read.rain == 0.5f);
    CHECK(read.wetness == 0.5f);
    CHECK(read.wind == wet.wind);
    CHECK(SceneWeather::fromJson(nlohmann::json(3)) == SceneWeather{});
  }

  TEST("clamped weather should keep each value in its range")
  {
    SceneWeather wild;
    wild.rain = 2.0f;
    wild.wetness = -1.0f;
    wild.puddles = std::numeric_limits<float>::quiet_NaN();
    wild.cloudDarkness = 1.5f;
    wild.wind = {std::numeric_limits<float>::infinity(), 1.0f};
    const SceneWeather tame = wild.clamped();
    CHECK(tame.rain == 1.0f);
    CHECK(tame.wetness == 0.0f);
    CHECK(tame.puddles == 0.0f);
    CHECK(tame.cloudDarkness == 1.0f);
    CHECK(tame.wind == glm::vec2(0.0f));
    // The wind is as strong as it is.
    wild.wind = {30.0f, -12.0f};
    CHECK(wild.clamped().wind == wild.wind);
  }

  TEST("clouds should drift with the wind, within their period")
  {
    // 10 m/s for 100 s: a kilometre.
    const glm::vec2 drift = driftClouds({0.0f, 0.0f}, {10.0f, 0.0f}, 100.0f);
    CHECK(std::abs(drift.x - 1.0f) < 1e-4f);
    CHECK(drift.y == 0.0f);
    // Backward past 0, and forward past the period: wrapped.
    const glm::vec2 back = driftClouds({0.5f, 0.0f}, {-10.0f, 0.0f}, 100.0f);
    CHECK(std::abs(back.x - (kCloudPeriod - 0.5f)) < 1e-3f);
    const glm::vec2 past =
        driftClouds({kCloudPeriod - 0.5f, 1.0f}, {0.0f, 10.0f}, 100.0f);
    CHECK(std::abs(past.x - (kCloudPeriod - 0.5f)) < 1e-3f);
    CHECK(std::abs(past.y - 2.0f) < 1e-4f);
    // Days of it: still within.
    glm::vec2 far{0.0f};
    for (int i = 0; i < 1000; ++i)
      far = driftClouds(far, {37.0f, -23.0f}, 600.0f);
    CHECK(far.x >= 0.0f);
    CHECK(far.x < kCloudPeriod);
    CHECK(far.y >= 0.0f);
    CHECK(far.y < kCloudPeriod);
    // Nonsense in: still a place in the pattern.
    const glm::vec2 nan = driftClouds(
        {1.0f, 1.0f}, {std::numeric_limits<float>::quiet_NaN(), 0.0f}, 1.0f);
    CHECK(nan == glm::vec2(1.0f, 1.0f));
  }

  return testing::report("weatherTests");
}
