#include "../../testHarness.h"

#include <engine/core/rendering/rain.h>

#include <cmath>
#include <limits>

using namespace sfs;

int main()
{
  TEST("rain settings should round-trip, missing keys kept")
  {
    const RainSettings defaults;
    CHECK(defaults.enabled);
    CHECK(RainSettings::fromJson(defaults.toJson()) == defaults);

    RainSettings custom;
    custom.enabled = false;
    custom.density = 0.5f;
    custom.length = 0.8f;
    custom.width = 3.0f;
    custom.brightness = 1.5f;
    custom.splashes = 0.25f;
    custom.ripples = 2.0f;
    custom.veil = 0.75f;
    custom.drips = 0.5f;
    CHECK(RainSettings::fromJson(custom.toJson()) == custom);

    const nlohmann::json partial = {{"density", 2.0f}, {"veil", "thick"}};
    const RainSettings read = RainSettings::fromJson(partial, custom);
    CHECK(read.density == 2.0f);
    CHECK(read.veil == custom.veil);
    CHECK(!read.enabled);
    CHECK(RainSettings::fromJson(nlohmann::json(3)) == RainSettings{});
  }

  TEST("streaks, splashes and the veil should grow with the rain")
  {
    const RainSettings rain;
    CHECK(rainStreaks(rain, 0.0f) == 0);
    CHECK(rainSplashes(rain, 0.0f) == 0);
    CHECK(rainVeilDensity(rain, 0.0f) == 0.0f);
    CHECK(rainStreaks(rain, 1.0f) == kRainStreaks);
    CHECK(rainSplashes(rain, 1.0f) == kRainSplashes);
    CHECK(rainStreaks(rain, 0.3f) < rainStreaks(rain, 0.6f));
    // A drizzle is much sparser than its share of a downpour.
    CHECK(rainStreaks(rain, 0.3f) < kRainStreaks / 6);
    CHECK(rainVeilDensity(rain, 0.3f) < rainVeilDensity(rain, 1.0f));
    // A downpour's veil hides about half of what's 150 m away.
    CHECK(std::abs(std::exp(-150.0f * rainVeilDensity(rain, 1.0f)) - 0.5f) <
          0.01f);
    // Out of range rain, or settings, stay bounded.
    CHECK(rainStreaks(rain, 5.0f) == kRainStreaks);
    CHECK(rainStreaks(rain, std::numeric_limits<float>::quiet_NaN()) == 0);
    RainSettings wild;
    wild.density = 1000.0f;
    CHECK(rainStreaks(wild, 1.0f) == 2 * kRainStreaks);

    RainSettings off;
    off.enabled = false;
    CHECK(rainStreaks(off, 1.0f) == 0);
    CHECK(rainSplashes(off, 1.0f) == 0);
    CHECK(rainVeilDensity(off, 1.0f) == 0.0f);
  }

  TEST("drips should come only in heavy rain, and linger slower after it")
  {
    const RainSettings rain;
    CHECK(rainDrips(rain, 0.0f, 0.0f).count == 0);
    CHECK(rainDrips(rain, 0.0f, 0.0f).line == 0.0f);
    // None in light rain, however wet.
    CHECK(rainDrips(rain, 0.3f, 0.4f).count == 0);
    const RainDrips pour = rainDrips(rain, 1.0f, 1.0f);
    CHECK(pour.count == kRainDrips);
    CHECK(pour.trickle == 1.0f);
    CHECK(pour.line == 1.0f);
    CHECK(rainDrips(rain, 0.6f, 0.6f).count < pour.count);
    CHECK(rainDrips(rain, 0.6f, 0.6f).trickle == 0.0f);
    // After the rain, soaked edges still drip: fewer, and slower.
    const RainDrips after = rainDrips(rain, 0.0f, 1.0f);
    CHECK(after.count > 0);
    CHECK(after.count < pour.count / 3);
    CHECK(after.rate < pour.rate);
    CHECK(after.trickle == 0.0f);
    CHECK(after.line > 0.0f);
    CHECK(rainDrips(rain, 0.0f, 0.3f).count == 0);
    // Bounded, and none with rain off or drips off.
    RainSettings wild;
    wild.drips = 1000.0f;
    CHECK(rainDrips(wild, 1.0f, 1.0f).count == 2 * kRainDrips);
    CHECK(
        rainDrips(rain, std::numeric_limits<float>::quiet_NaN(), 0.0f).count ==
        0);
    RainSettings none;
    none.drips = 0.0f;
    CHECK(rainDrips(none, 1.0f, 1.0f).count == 0);
    CHECK(rainDrips(none, 1.0f, 1.0f).line == 0.0f);
    RainSettings off;
    off.enabled = false;
    CHECK(rainDrips(off, 1.0f, 1.0f).count == 0);
  }

  TEST("rain's drift should follow the wind it's had, eased, within its "
       "period")
  {
    RainDrift drift;
    drift.advance({10.0f, 0.0f}, 1.0f);
    CHECK(drift.wind == glm::vec2(10.0f, 0.0f)); // from the first wind
    CHECK(std::abs(drift.drift.x - 8.0f) < 1e-4f);
    // A veer: the drops' wind turns toward it over a second or so.
    drift.advance({0.0f, 10.0f}, 0.1f);
    CHECK(drift.wind.x > 8.0f);
    CHECK(drift.wind.y > 0.0f);
    CHECK(drift.wind.y < 2.0f);
    for (int i = 0; i < 100; ++i)
      drift.advance({0.0f, 10.0f}, 0.1f);
    CHECK(std::abs(drift.wind.y - 10.0f) < 0.01f);
    // Wrapped into its period, never negative.
    for (int i = 0; i < 200; ++i)
      drift.advance({-30.0f, 0.0f}, 1.0f);
    CHECK(drift.drift.x >= 0.0f);
    CHECK(drift.drift.x < kRainDriftPeriod);
    // Nonsense changes nothing.
    const RainDrift before = drift;
    drift.advance({std::nanf(""), 0.0f}, 1.0f);
    CHECK(drift.drift == before.drift);
  }

  TEST("the overhead map should lie ahead of the camera, over it")
  {
    const glm::vec3 camera(10.0f, -20.0f, 15.0f);
    const RainMap map = rainMapFor(camera, glm::vec3(0.0f, 0.9f, -0.4f));
    // Ahead (along +Y), the camera still inside.
    CHECK(map.center.y > camera.y);
    CHECK(std::abs(map.center.x - camera.x) < 0.1f);
    CHECK(std::abs(camera.y - map.center.y) < map.halfSize);
    CHECK(map.top > camera.z);

    // The camera's ground maps into the square, depth 0 at the top, 1 its
    // range below.
    const glm::vec4 ground =
        map.viewProjection * glm::vec4(camera.x, camera.y, 0.0f, 1.0f);
    CHECK(std::abs(ground.x) < 1.0f && std::abs(ground.y) < 1.0f);
    CHECK(std::abs(ground.z - map.top / map.depthRange) < 1e-4f);
    const glm::vec4 top =
        map.viewProjection * glm::vec4(map.center, map.top, 1.0f);
    CHECK(std::abs(top.x) < 1e-4f && std::abs(top.y) < 1e-4f);
    CHECK(std::abs(top.z) < 1e-4f);

    // Snapped to whole texels, so moving a little doesn't shift its texels.
    const float texel = 2.0f * kRainMapHalfSize / kRainMapResolution;
    const float steps = map.center.x / texel;
    CHECK(std::abs(steps - std::round(steps)) < 1e-3f);

    // Looking straight down: centred on the camera.
    const RainMap down = rainMapFor(camera, glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(glm::length(down.center - glm::vec2(camera)) < texel);
  }

  return testing::report("rainTests");
}
