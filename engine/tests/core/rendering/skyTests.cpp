#include "../../testHarness.h"

#include <engine/core/rendering/lighting.h>
#include <engine/core/rendering/sky.h>
#include <engine/core/rendering/sunShadow.h>

#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"

#include <cmath>

using namespace sfs;

namespace
{

// The sun `degrees` above the horizon, toward +X (a little south).
glm::vec3 sunAt(float degrees)
{
  const float elevation = glm::radians(degrees);
  return glm::normalize(glm::vec3(
      std::cos(elevation), -0.3f * std::cos(elevation), std::sin(elevation)));
}

float luminance(const glm::vec3& c)
{
  return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

glm::vec3 project(const glm::mat4& m, const glm::vec3& p)
{
  const glm::vec4 clip = m * glm::vec4(p, 1.0f);
  return glm::vec3(clip) / clip.w;
}

} // namespace

int main()
{
  TEST("sky settings should default to a flat sky, and round-trip")
  {
    CHECK(SkySettings{}.model == SkyModel::Flat);
    CHECK(!SkySettings{}.physical());

    SkySettings sky;
    sky.model = SkyModel::Physical;
    sky.turbidity = 3.5f;
    sky.groundAlbedo = 0.2f;
    sky.sunSize = 2.0f;
    sky.intensity = 1.25f;
    sky.nightBrightness = 0.5f;
    sky.adaptation = 0.8f;
    CHECK(SkySettings::fromJson(sky.toJson()) == sky);
    CHECK(sky.toJson()["model"] == "physical");
    // Missing keys keep the defaults given; an unknown model keeps its.
    const SkySettings partial =
        SkySettings::fromJson({{"turbidity", 6.0}, {"model", "nope"}}, sky);
    CHECK(partial.model == SkyModel::Physical);
    CHECK(testing::approx(partial.turbidity, 6.0f));
    CHECK(testing::approx(partial.sunSize, 2.0f));
    CHECK(skyModelFromName("flat") == SkyModel::Flat);
    CHECK(!skyModelFromName("cloudy"));
  }

  const PhysicalSky sky;

  TEST("the air should let more light through looking up than along the "
       "horizon, and none through the ground")
  {
    const glm::vec3 up = sky.transmittance(0.1f, 1.0f);
    const glm::vec3 low = sky.transmittance(0.1f, 0.05f);
    const glm::vec3 down = sky.transmittance(0.1f, -0.5f);
    CHECK(up.r > 0.85f && up.b > 0.6f);
    CHECK(low.r < up.r && low.b < up.b);
    CHECK(low.b < low.r); // blue scatters most
    CHECK(luminance(down) < 1e-3f);
    // Higher up, more gets through.
    CHECK(sky.transmittance(20.0f, 0.05f).b > low.b);
  }

  TEST("the sun should be bright and near white at noon, warm and dim at "
       "sunset, and gone at night")
  {
    const glm::vec3 noon = sky.sunLight(sunAt(60.0f));
    const glm::vec3 sunset = sky.sunLight(sunAt(3.0f));
    const glm::vec3 night = sky.sunLight(sunAt(-10.0f));
    CHECK(luminance(noon) > 0.6f && luminance(noon) < 1.3f);
    CHECK(noon.b > 0.7f * noon.r);
    CHECK(luminance(sunset) < luminance(noon) * 0.6f);
    CHECK(sunset.r > 2.0f * sunset.b); // orange
    CHECK(luminance(night) == 0.0f);
    // Rising, it comes in gradually: a little as its disk clears the
    // horizon, more a degree on, all of it from 5 degrees up.
    const float rising = luminance(sky.sunLight(sunAt(0.3f)));
    const float risen = luminance(sky.sunLight(sunAt(1.5f)));
    CHECK(rising > 0.0f && rising < risen * 0.5f);
    CHECK(PhysicalSky::clearOfHorizon(sunAt(6.0f)) == 1.0f);
    CHECK(PhysicalSky::clearOfHorizon(sunAt(-1.0f)) == 0.0f);
  }

  TEST("the sky should be blue overhead by day, glow warm toward a setting "
       "sun, and be dark at night")
  {
    const glm::vec3 zenith = sky.radiance({0, 0, 1}, sunAt(45.0f));
    CHECK(zenith.b > zenith.g && zenith.g > zenith.r);
    CHECK(luminance(zenith) > 0.02f && luminance(zenith) < 0.3f);
    // By day the horizon is paler (whiter) than the zenith.
    const glm::vec3 horizon =
        sky.radiance(glm::normalize(glm::vec3(-1, 0.4f, 0.05f)), sunAt(45.0f));
    CHECK(horizon.r / horizon.b > zenith.r / zenith.b);

    // At sunset: toward the sun, the horizon's warm and bright; the sky
    // overhead dimmer than by day.
    const glm::vec3 sun = sunAt(2.0f);
    const glm::vec3 glow =
        sky.radiance(glm::normalize(glm::vec3(sun.x, sun.y, 0.03f)), sun);
    CHECK(glow.r > glow.b * 2.0f);
    CHECK(luminance(glow) > luminance(zenith));
    CHECK(luminance(sky.radiance({0, 0, 1}, sun)) < luminance(zenith));

    // At night: dark, a little blue (the night glow).
    const glm::vec3 night = sky.radiance({0, 0, 1}, sunAt(-15.0f));
    CHECK(luminance(night) < 0.01f);
    CHECK(night.b > night.r);
    SkySettings black;
    black.nightBrightness = 0.0f;
    CHECK(luminance(PhysicalSky(black).radiance({0, 0, 1}, sunAt(-15.0f))) <
          1e-3f);
  }

  TEST("the moon should light a little and brighten the night sky, more the "
       "fuller and higher it is")
  {
    const glm::vec3 high = glm::normalize(glm::vec3(0.2f, -0.3f, 0.9f));
    const glm::vec3 low = glm::normalize(glm::vec3(0.9f, -0.3f, 0.06f));
    const glm::vec3 full = sky.moonLight(high, 1.0f);
    CHECK(full.b > full.r); // bluish
    CHECK(luminance(full) > 0.002f && luminance(full) < 0.02f);
    CHECK(luminance(sky.moonLight(high, 0.25f)) < luminance(full));
    CHECK(luminance(sky.moonLight(low, 1.0f)) < luminance(full));

    const float night = PhysicalSky::nightScale(1.0f);
    CHECK(night > 1.0f);
    CHECK(PhysicalSky::nightScale(0.0f) == 1.0f);
    CHECK(luminance(sky.radiance({0, 0, 1}, sunAt(-15.0f), night)) >
          luminance(sky.radiance({0, 0, 1}, sunAt(-15.0f))));
  }

  TEST("the moon's share of the light should grow with its phase and "
       "height, fade in as the sun sinks, and dim behind clouds")
  {
    SceneLighting lighting;
    lighting.sun.direction = sunAt(-15.0f);
    lighting.moon.direction = {0.0f, -0.3f, 0.9f};
    lighting.moon.phase = 1.0f;
    const float full = moonlight(lighting);
    CHECK(full > 0.9f);
    lighting.moon.phase = 0.5f;
    CHECK(moonlight(lighting) < full * 0.5f);
    lighting.moon.phase = 1.0f;
    lighting.cloudCover = 1.0f;
    CHECK(moonlight(lighting) < full * 0.3f);
    lighting.cloudCover = 0.0f;
    // The sun just set: hardly yet; still up: none.
    lighting.sun.direction = sunAt(-1.0f);
    CHECK(moonlight(lighting) < full * 0.3f);
    lighting.sun.direction = sunAt(5.0f);
    CHECK(moonlight(lighting) == 0.0f);
    // Below the horizon: none.
    lighting.sun.direction = sunAt(-15.0f);
    lighting.moon.direction = {0.0f, -0.3f, -0.1f};
    CHECK(moonlight(lighting) == 0.0f);
  }

  TEST("hazier air should pale the sky and dim the sun")
  {
    SkySettings hazy;
    hazy.turbidity = 8.0f;
    const PhysicalSky thick(hazy);
    const glm::vec3 clear = sky.radiance({0, 0, 1}, sunAt(45.0f));
    const glm::vec3 pale = thick.radiance({0, 0, 1}, sunAt(45.0f));
    CHECK(pale.r / pale.b > clear.r / clear.b);
    CHECK(luminance(thick.sunLight(sunAt(20.0f))) <
          luminance(sky.sunLight(sunAt(20.0f))));
  }

  TEST("the sky's diffuse light should come mostly from above by day, and "
       "be faint at night")
  {
    const SkyIrradiance day = sky.irradiance(sunAt(45.0f));
    const glm::vec3 up = day.at({0, 0, 1});
    const glm::vec3 down = day.at({0, 0, -1});
    CHECK(up.b > up.r); // blue sky
    CHECK(luminance(up) > 0.03f && luminance(up) < 0.5f);
    // The ground's bounce lights what faces down, less than the sky.
    CHECK(luminance(down) > 0.0f);
    // Toward the sun's side, brighter than away from it.
    const glm::vec3 sun = sunAt(45.0f);
    CHECK(luminance(day.at(glm::normalize(glm::vec3(sun.x, sun.y, 0)))) >
          luminance(day.at(glm::normalize(glm::vec3(-sun.x, -sun.y, 0)))));

    const SkyIrradiance night = sky.irradiance(sunAt(-15.0f));
    CHECK(luminance(night.at({0, 0, 1})) < 0.01f);
  }

  TEST("eyes should adapt to a dim sky by a share of the stops it's dimmer, "
       "up to three, and not at all by day")
  {
    const auto adapted = [](const PhysicalSky& model, float degrees)
    {
      const glm::vec3 sun = sunAt(degrees);
      return model.adaptation(
          sun, model.sunLight(sun), model.irradiance(sun).at({0, 0, 1}));
    };
    CHECK(testing::approx(adapted(sky, 60.0f), 1.0f, 0.05f));
    const float dusk = adapted(sky, 4.0f);
    const float night = adapted(sky, -20.0f);
    CHECK(dusk > 1.3f && dusk < night);
    CHECK(testing::approx(night, 8.0f, 1e-3f) || night < 8.0f);
    SkySettings fixed;
    fixed.adaptation = 0.0f;
    CHECK(adapted(PhysicalSky(fixed), -20.0f) == 1.0f);
  }

  TEST("lamps should look as bright as they are at night, and dimmer as eyes "
       "adapt to daylight; their shadows only once they show")
  {
    const auto lamps = [](const PhysicalSky& model, float degrees)
    {
      const glm::vec3 sun = sunAt(degrees);
      return model.lampScale(model.adaptation(
          sun, model.sunLight(sun), model.irradiance(sun).at({0, 0, 1})));
    };
    CHECK(testing::approx(sky.mostAdaptation(), PhysicalSky::kMostAdaptation));
    CHECK(testing::approx(sky.lampScale(PhysicalSky::kMostAdaptation), 1.0f));
    CHECK(testing::approx(lamps(sky, 60.0f), 0.125f, 0.01f));
    CHECK(lamps(sky, 4.0f) > lamps(sky, 60.0f));
    CHECK(lamps(sky, -20.0f) > lamps(sky, 4.0f));
    // Eyes that don't adapt see lamps as they are, day and night.
    SkySettings fixed;
    fixed.adaptation = 0.0f;
    const PhysicalSky still(fixed);
    CHECK(still.mostAdaptation() == 1.0f);
    CHECK(lamps(still, 60.0f) == 1.0f);

    CHECK(PhysicalSky::lampShadow(1.0f, 0.125f) == 0.0f);
    CHECK(PhysicalSky::lampShadow(1.0f, 1.0f) == 1.0f);
    const float fading = PhysicalSky::lampShadow(1.0f, 0.3f);
    CHECK(fading > 0.0f && fading < 1.0f);
  }

  TEST("the BRDF table should reflect everything face-on when smooth, and "
       "less when rough")
  {
    const int size = 16;
    const std::vector<glm::vec2> table = brdfTable(size);
    CHECK(table.size() == std::size_t(size * size));
    const glm::vec2 smoothFaceOn = table[size - 1]; // n.v ~ 1, roughness ~ 0
    CHECK(smoothFaceOn.x > 0.9f && smoothFaceOn.x < 1.05f);
    CHECK(smoothFaceOn.y < 0.05f);
    const glm::vec2 roughFaceOn = table[(size - 1) * size + size - 1];
    CHECK(roughFaceOn.x < smoothFaceOn.x);
    // Grazing: more of the Fresnel bias.
    CHECK(table[0].y > smoothFaceOn.y);
    for (const glm::vec2& texel : table)
      CHECK(texel.x >= 0.0f && texel.y >= 0.0f && texel.x + texel.y <= 1.05f);
  }

  TEST("cascade splits should run from near to far, closer together near the "
       "camera")
  {
    const auto splits = cascadeSplits(4, 0.1f, 200.0f, 0.75f);
    CHECK(testing::approx(splits[0], 0.1f));
    CHECK(testing::approx(splits[4], 200.0f));
    for (int i = 0; i < 4; ++i)
      CHECK(splits[i] < splits[i + 1]);
    CHECK(splits[1] - splits[0] < splits[2] - splits[1]);
    CHECK(splits[3] - splits[2] < splits[4] - splits[3]);
    // Lambda 0: even; 1: logarithmic.
    const auto even = cascadeSplits(4, 0.1f, 200.0f, 0.0f);
    CHECK(testing::approx(even[2], 100.05f, 1e-2f));
    const auto logarithmic = cascadeSplits(2, 1.0f, 100.0f, 1.0f);
    CHECK(testing::approx(logarithmic[1], 10.0f, 1e-3f));
    // Fewer cascades: the rest at the far plane.
    const auto two = cascadeSplits(2, 0.1f, 50.0f, 0.5f);
    CHECK(testing::approx(two[2], 50.0f) && testing::approx(two[4], 50.0f));
  }

  CascadeView view;
  view.position = {3.0f, -7.0f, 4.0f};
  view.forward = glm::normalize(glm::vec3(0.3f, 1.0f, -0.4f));
  view.halfExtent = {std::tan(0.4f) * 16.0f / 9.0f, std::tan(0.4f)};
  view.nearPlane = 0.1f;
  view.farPlane = 500.0f;
  const glm::vec3 sun = sunAt(35.0f);

  TEST("each cascade should cover its slice of the view")
  {
    const SunCascades cascades =
        fitSunCascades(sun, view, 4, 200.0f, 0.75f, 2048);
    CHECK(cascades.count == 4);
    CHECK(testing::approx(cascades.cascades[3].splitFar, 200.0f));
    const glm::vec3 right =
        glm::normalize(glm::cross(view.forward, glm::vec3(0, 0, 1)));
    const glm::vec3 up = glm::cross(right, view.forward);
    for (uint32_t i = 0; i < cascades.count; ++i)
    {
      const SunCascade& cascade = cascades.cascades[i];
      // The slice's corners, near and far, inside its box (and depth).
      for (const float d : {cascade.splitNear, cascade.splitFar})
        for (const float sx : {-1.0f, 1.0f})
          for (const float sy : {-1.0f, 1.0f})
          {
            const glm::vec3 corner = view.position + view.forward * d +
                                     right * (sx * d * view.halfExtent.x) +
                                     up * (sy * d * view.halfExtent.y);
            const glm::vec3 ndc = project(cascade.viewProjection, corner);
            CHECK(std::abs(ndc.x) <= 1.001f && std::abs(ndc.y) <= 1.001f);
            CHECK(ndc.z > 0.0f && ndc.z < 1.0f);
          }
      // Farther cascades are bigger.
      if (i > 0)
        CHECK(cascade.radius > cascades.cascades[i - 1].radius);
    }
    // A caster well toward the sun from the nearest still casts into it.
    const glm::vec3 caster = cascades.cascades[0].center + sun * 60.0f;
    const glm::vec3 ndc = project(cascades.cascades[0].viewProjection, caster);
    CHECK(ndc.z > 0.0f);

    // One view-projection around them all.
    const glm::mat4 bounds = cascadeBounds(sun, cascades);
    for (uint32_t i = 0; i < cascades.count; ++i)
    {
      const glm::vec3 center = project(bounds, cascades.cascades[i].center);
      CHECK(std::abs(center.x) < 1.0f && std::abs(center.y) < 1.0f);
      CHECK(center.z > 0.0f && center.z < 1.0f);
    }
  }

  TEST("cascades should keep their size as the camera turns, and move only "
       "by whole texels as it moves (no shimmering)")
  {
    const uint32_t resolution = 2048;
    const SunCascades a = fitSunCascades(sun, view, 4, 200.0f, 0.75f, 2048);
    CascadeView turned = view;
    turned.forward = glm::normalize(glm::vec3(-0.8f, 0.2f, -0.3f));
    const SunCascades b = fitSunCascades(sun, turned, 4, 200.0f, 0.75f, 2048);
    for (uint32_t i = 0; i < 4; ++i)
      CHECK(a.cascades[i].radius == b.cascades[i].radius);

    CascadeView moved = view;
    moved.position += glm::vec3(0.0137f, -0.0213f, 0.002f);
    const SunCascades c = fitSunCascades(sun, moved, 4, 200.0f, 0.75f, 2048);
    for (uint32_t i = 0; i < 4; ++i)
    {
      // A fixed world point shifts across each cascade by whole texels.
      const glm::vec3 point{5.0f, 9.0f, 0.5f};
      const glm::vec3 before = project(a.cascades[i].viewProjection, point);
      const glm::vec3 after = project(c.cascades[i].viewProjection, point);
      const float texelsX =
          (after.x - before.x) * static_cast<float>(resolution) * 0.5f;
      const float texelsY =
          (after.y - before.y) * static_cast<float>(resolution) * 0.5f;
      CHECK(testing::approx(texelsX, std::round(texelsX), 2e-2f));
      CHECK(testing::approx(texelsY, std::round(texelsY), 2e-2f));
    }
  }

  TEST("cascades should stop at the far plane if it's nearer than their "
       "reach")
  {
    CascadeView shortView = view;
    shortView.farPlane = 60.0f;
    const SunCascades cascades =
        fitSunCascades(sun, shortView, 3, 200.0f, 0.5f, 1024);
    CHECK(cascades.count == 3);
    CHECK(testing::approx(cascades.cascades[2].splitFar, 60.0f));
    // An orthographic view's slices are boxes.
    CascadeView ortho = view;
    ortho.perspective = false;
    ortho.halfExtent = {8.0f, 4.5f};
    const glm::vec2 sphere = cascadeSphere(ortho, 0.0f, 10.0f);
    CHECK(testing::approx(sphere.x, 5.0f));
    CHECK(testing::approx(
        sphere.y, std::sqrt(8.0f * 8.0f + 4.5f * 4.5f + 25.0f), 1e-3f));
  }

  return testing::report("skyTests");
}
