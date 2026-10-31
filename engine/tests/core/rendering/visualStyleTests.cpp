#include "../../testHarness.h"

#include <engine/core/rendering/ambientOcclusion.h>
#include <engine/core/rendering/colorGrading.h>
#include <engine/core/rendering/contactShadows.h>
#include <engine/core/rendering/fog.h>
#include <engine/core/rendering/visualStyle.h>

#include <cmath>

#include "glm/glm/geometric.hpp"
#include "glm/glm/matrix.hpp"

using namespace sfs;

int main()
{
  TEST("colour grading should default to changing nothing, and round-trip")
  {
    CHECK(ColorGrading{}.neutral());
    CHECK(ColorGrading{}.toneMapper == ToneMapper::None);

    ColorGrading grading;
    grading.toneMapper = ToneMapper::AgX;
    grading.exposure = 0.5f;
    grading.temperature = -0.25f;
    grading.tint = 0.1f;
    grading.contrast = 1.2f;
    grading.saturation = 0.8f;
    grading.lift = {0.01f, 0.0f, 0.02f};
    grading.gamma = {1.1f, 1.0f, 0.9f};
    grading.gain = {1.0f, 0.95f, 0.9f};
    grading.vignette = 0.3f;
    grading.vignetteSoftness = 0.7f;
    grading.autoExposure = true;
    grading.exposureTarget = 0.25f;
    grading.exposureDarken = 2.0f;
    grading.exposureBrighten = 0.0f;
    grading.exposureSpeed = 3.0f;
    CHECK(!grading.neutral());
    CHECK(ColorGrading::fromJson(grading.toJson()) == grading);
    CHECK(grading.toJson()["toneMapper"] == "agx");

    // Missing keys keep the defaults given; an unknown mapper keeps its.
    const ColorGrading partial = ColorGrading::fromJson(
        {{"exposure", 1.0}, {"toneMapper", "nope"}}, grading);
    CHECK(partial.toneMapper == ToneMapper::AgX);
    CHECK(testing::approx(partial.exposure, 1.0f));
    CHECK(testing::approx(partial.contrast, 1.2f));
  }

  TEST("every tone mapper should have a name that reads back")
  {
    for (const ToneMapper mapper : {ToneMapper::None,
                                    ToneMapper::Aces,
                                    ToneMapper::AgX,
                                    ToneMapper::Neutral})
      CHECK(toneMapperFromName(toneMapperName(mapper)) == mapper);
    CHECK(!toneMapperFromName("filmic").has_value());
  }

  TEST("white balance should be the identity at 0, and warm when warmer")
  {
    CHECK(whiteBalanceMatrix(0.0f, 0.0f) == glm::mat3(1.0f));
    // White, warmed: more red than blue; cooled: the other way.
    const glm::vec3 warm = whiteBalanceMatrix(0.5f, 0.0f) * glm::vec3(1.0f);
    const glm::vec3 cool = whiteBalanceMatrix(-0.5f, 0.0f) * glm::vec3(1.0f);
    CHECK(warm.r > warm.b + 0.05f);
    CHECK(cool.b > cool.r + 0.05f);
    // Tint: magenta (+) has less green than its red and blue.
    const glm::vec3 magenta = whiteBalanceMatrix(0.0f, 0.5f) * glm::vec3(1.0f);
    CHECK(magenta.g < magenta.r && magenta.g < magenta.b);
  }

  TEST("a visual style should round-trip, its optional parts only when set")
  {
    VisualStyle plain;
    const nlohmann::json plainJson = plain.toJson();
    CHECK(!plainJson.contains("bloom"));
    CHECK(!plainJson.contains("haze"));
    CHECK(!plainJson.contains("shadows"));
    CHECK(VisualStyle::fromJson(plainJson) == plain);
    CHECK(plain.lightBalance().neutral());

    VisualStyle style;
    style.grading.toneMapper = ToneMapper::Aces;
    style.grading.exposure = 0.4f;
    style.sunIntensity = 1.5f;
    style.sunTint = {1.0f, 0.9f, 0.8f};
    style.ambientIntensity = 0.8f;
    style.ambientTint = {0.85f, 0.95f, 1.1f};
    style.skyIntensity = 1.2f;
    style.bloom = BloomSettings{.intensity = 0.3f, .threshold = 1.6f};
    style.haze = HazeSettings{0.25f, 150.0f};
    style.shadowSoftness = 2.5f;
    style.materialVariant = "realistic";
    const std::optional<VisualStyle> loaded =
        VisualStyle::fromJson(style.toJson());
    CHECK(loaded && *loaded == style);
    CHECK(testing::approx(style.lightBalance().sun.g, 1.35f));
    CHECK(testing::approx(style.lightBalance().ambient.b, 0.88f));
    CHECK(testing::approx(style.skyScale().r, 1.2f));

    CHECK(!VisualStyle::fromJson(nlohmann::json::array()).has_value());
    // An empty object: a style that changes nothing.
    CHECK(VisualStyle::fromJson(nlohmann::json::object()) == plain);
  }

  TEST("a style's sky and each shadow setting should round-trip, and replace "
       "the scene's only when set")
  {
    // A style from before skies and cascades: none of them, and saved again
    // just as it was.
    const nlohmann::json old = {{"formatVersion", 1},
                                {"grading", {{"toneMapper", "aces"}}},
                                {"shadows", {{"softness", 2.6}}},
                                {"materialVariant", "realistic"}};
    const std::optional<VisualStyle> older = VisualStyle::fromJson(old);
    CHECK(older && !older->sky && !older->shadowCascades &&
          !older->shadowCascadeDistance && !older->shadowCascadeSplit);
    CHECK(older &&
          older->toJson()["shadows"] == nlohmann::json({{"softness", 2.6f}}));
    CHECK(older && !older->toJson().contains("sky"));

    ShadowSettings scene;
    scene.softness = 1.0f;
    scene.distance = 55.0f;
    CHECK(VisualStyle{}.shadows(scene) == scene);

    VisualStyle style;
    style.shadowCascades = 4;
    style.shadowCascadeDistance = 220.0f;
    style.sky = SkySettings{.model = SkyModel::Physical, .turbidity = 3.0f};
    const std::optional<VisualStyle> loaded =
        VisualStyle::fromJson(style.toJson());
    CHECK(loaded && *loaded == style);
    CHECK(loaded && !loaded->toJson()["shadows"].contains("softness"));
    const ShadowSettings mixed = style.shadows(scene);
    CHECK(mixed.cascades == 4 &&
          testing::approx(mixed.cascadeDistance, 220.0f));
    CHECK(testing::approx(mixed.softness, 1.0f)); // the scene's
    CHECK(testing::approx(mixed.distance, 55.0f));
    CHECK(testing::approx(mixed.cascadeSplit, scene.cascadeSplit));

    // Asking for more cascades than there can be gets the most.
    const std::optional<VisualStyle> many =
        VisualStyle::fromJson({{"shadows", {{"cascades", 9}}}});
    CHECK(many && many->shadowCascades == 4u);
  }

  TEST("a style's ambient occlusion and fog should round-trip, only when set")
  {
    VisualStyle none;
    CHECK(!none.toJson().contains("ambientOcclusion"));
    CHECK(!none.toJson().contains("fog"));
    const std::optional<VisualStyle> plain = VisualStyle::fromJson(
        {{"formatVersion", 1}, {"grading", {{"toneMapper", "aces"}}}});
    CHECK(plain && !plain->ambientOcclusion && !plain->fog);

    VisualStyle style;
    style.ambientOcclusion = AmbientOcclusionSettings{
        .enabled = true, .radius = 1.5f, .intensity = 0.8f, .samples = 16};
    style.fog = FogSettings{.enabled = true,
                            .density = 0.01f,
                            .heightFalloff = 0.3f,
                            .baseHeight = -2.0f,
                            .startDistance = 10.0f,
                            .maxOpacity = 0.7f,
                            .sunGlow = 0.4f};
    style.temporalAntiAliasing = true;
    const std::optional<VisualStyle> loaded =
        VisualStyle::fromJson(style.toJson());
    CHECK(loaded && *loaded == style);

    // Missing keys keep their defaults; samples are kept within bounds.
    const std::optional<VisualStyle> partial = VisualStyle::fromJson(
        {{"ambientOcclusion", {{"enabled", true}, {"samples", 100}}},
         {"fog", {{"density", 0.05}}}});
    CHECK(partial && partial->ambientOcclusion && partial->fog);
    CHECK(partial->ambientOcclusion->samples == kMaxOcclusionSamples);
    CHECK(testing::approx(partial->ambientOcclusion->radius, 1.0f));
    CHECK(!partial->fog->enabled); // as the default

    // Contact shadows, the same way.
    CHECK(!none.toJson().contains("contactShadows"));
    CHECK(!plain->contactShadows);
    VisualStyle contact;
    contact.contactShadows = ContactShadowSettings{
        .enabled = true, .length = 0.7f, .thickness = 0.4f, .strength = 0.5f};
    const std::optional<VisualStyle> reloaded =
        VisualStyle::fromJson(contact.toJson());
    CHECK(reloaded && *reloaded == contact);
    const std::optional<VisualStyle> partialContact =
        VisualStyle::fromJson({{"contactShadows", {{"enabled", true}}}});
    CHECK(partialContact && partialContact->contactShadows);
    CHECK(partialContact->contactShadows->enabled);
    CHECK(testing::approx(partialContact->contactShadows->length,
                          ContactShadowSettings{}.length));

    // Material overrides, by name, only when there are some.
    CHECK(!none.toJson().contains("materials"));
    VisualStyle overriding;
    overriding.materials["game:ground"] =
        "asset:00000000000000000000000000000001";
    const std::optional<VisualStyle> reread =
        VisualStyle::fromJson(overriding.toJson());
    CHECK(reread && reread->materials == overriding.materials);
    CHECK(testing::approx(partial->fog->density, 0.05f));
  }

  TEST("height fog should thicken with distance, thin with height, start "
       "where it starts and stop at its most")
  {
    FogSettings fog;
    CHECK(fogOpacity(fog, {0, 0, 1}, {100, 0, 1}) == 0.0f); // off
    fog.enabled = true;
    fog.density = 0.02f;
    fog.heightFalloff = 0.0f;
    // Uniform: Beer-Lambert along the line.
    CHECK(testing::approx(
        fogOpacity(fog, {0, 0, 0}, {50, 0, 0}), 1.0f - std::exp(-1.0f), 1e-4f));
    CHECK(fogOpacity(fog, {0, 0, 0}, {10, 0, 0}) <
          fogOpacity(fog, {0, 0, 0}, {100, 0, 0}));

    // Exponential in height: level at 10 m up, e^-2 as thick (falloff 0.2).
    fog.heightFalloff = 0.2f;
    const float low = fogOpacity(fog, {0, 0, 0}, {50, 0, 0});
    const float high = fogOpacity(fog, {0, 0, 10}, {50, 0, 10});
    CHECK(testing::approx(high, 1.0f - std::exp(-std::exp(-2.0f)), 1e-4f));
    CHECK(high < low);
    // A sloped line: the integral of the density along it (by sums).
    const glm::vec3 camera{0, 0, 12};
    const glm::vec3 point{60, 0, 0};
    double depth = 0.0;
    const int steps = 20000;
    for (int i = 0; i < steps; ++i)
    {
      const glm::vec3 at = camera + (point - camera) * ((i + 0.5f) / steps);
      depth +=
          0.02 * std::exp(-0.2 * at.z) * (glm::length(point - camera) / steps);
    }
    CHECK(testing::approx(fogOpacity(fog, camera, point),
                          static_cast<float>(1.0 - std::exp(-depth)),
                          1e-3f));
    // Looking straight up and down, too (far below the base it's thick).
    CHECK(fogOpacity(fog, {0, 0, 0}, {0, 0, 1000}) > 0.0f);
    CHECK(fogOpacity(fog, {0, 0, 0}, {0, 0, 1000}) < 0.1f);
    CHECK(std::isfinite(fogOpacity(fog, {0, 0, 0}, {0, 0, -1000})));

    // Nothing nearer than where it starts; never more than its most.
    fog.startDistance = 20.0f;
    CHECK(fogOpacity(fog, {0, 0, 0}, {15, 0, 0}) == 0.0f);
    CHECK(fogOpacity(fog, {0, 0, 0}, {30, 0, 0}) <
          fogOpacity(FogSettings{.enabled = true, .heightFalloff = 0.2f},
                     {0, 0, 0},
                     {30, 0, 0}));
    fog.maxOpacity = 0.4f;
    CHECK(testing::approx(fogOpacity(fog, {0, 0, 0}, {5000, 0, 0}), 0.4f));
  }

  TEST("the fog's glow should be brightest toward the sun, and integrate to "
       "1 over the sphere")
  {
    CHECK(fogPhase(1.0f) > fogPhase(0.5f));
    CHECK(fogPhase(0.5f) > fogPhase(-1.0f));
    double sum = 0.0;
    const int steps = 4000;
    for (int i = 0; i < steps; ++i)
    {
      const double theta = 3.14159265358979 * (i + 0.5) / steps;
      sum += fogPhase(static_cast<float>(std::cos(theta))) * std::sin(theta) *
             2.0 * 3.14159265358979 * (3.14159265358979 / steps);
    }
    CHECK(testing::approx(static_cast<float>(sum), 1.0f, 1e-2f));
  }
}
