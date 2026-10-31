#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "nlohmann/json.hpp"

#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace sfs
{

/**
 * What the sky is.
 *
 * - Flat: the scene's sky colour (SceneLighting::skyColor) behind everything,
 *   and the engine's classic lighting: the sun's colour and strength, and the
 *   ambient level, as the scene sets them. The default.
 * - Physical: an atmosphere lit by the sun (PhysicalSky): blue by day, warm
 *   at sunrise and sunset, dark blue at night, with the sun's disk. The sun's
 *   colour and strength come from how much of its light gets through the air
 *   (red at sunset, none at night), and everything is lit by the sky around
 *   it (image-based lighting: its diffuse light and reflections), the way a
 *   physically based renderer lights an outdoor scene. Only the sun's
 *   direction is taken from the scene; its ambient level only fades point
 *   lights in, as ever.
 */
enum class SkyModel
{
  Flat,
  Physical,
};

/** "flat" or "physical". */
const char* skyModelName(SkyModel model);
std::optional<SkyModel> skyModelFromName(std::string_view name);

/**
 * The sky (see SkyModel). Only `model` matters to a flat sky; the rest are a
 * physical sky's.
 *
 * @param turbidity     how hazy the air is: 1 very clear, 2 a clear day, 4-6
 *                      hazy, 10 thick haze (paler sky, softer sun, a wider
 *                      glow around it)
 * @param groundAlbedo  how much light the ground reflects (0..1): what's
 *                      below the horizon in reflections, and the light it
 *                      bounces up
 * @param sunSize       the sun's disk, x the real sun's size (0: none drawn;
 *                      the light is the same)
 * @param intensity     how bright the sun and sky are, x
 * @param nightBrightness the night sky's glow (moon- and starlight, airglow),
 *                      x; 0 a black night
 * @param adaptation    how much eyes adapt as the sky's light dims (dusk,
 *                      night): 0 not at all (a fixed exposure, physically
 *                      dark evenings), 1 fully (up to 3 stops). It brightens
 *                      the sky's own light (sun, sky, reflections, the
 *                      background), not point lights or glowing things
 */
struct SkySettings
{
  SkyModel model = SkyModel::Flat;
  float turbidity = 2.0f;
  float groundAlbedo = 0.3f;
  float sunSize = 1.0f;
  float intensity = 1.0f;
  float nightBrightness = 1.0f;
  float adaptation = 0.5f;

  bool physical() const { return model == SkyModel::Physical; }

  nlohmann::json toJson() const;
  /** Missing keys keep `defaults`' values. */
  static SkySettings fromJson(const nlohmann::json& json,
                              const SkySettings& defaults);
  static SkySettings fromJson(const nlohmann::json& json);

  bool operator==(const SkySettings&) const = default;
};

/**
 * Diffuse light from all around, as 9 spherical-harmonic coefficients (rgb):
 * at(normal) is the light a surface facing `normal` receives, already
 * convolved with the cosine lobe and divided by pi (so albedo x at(n) is its
 * diffuse colour). Evaluated by the lit shader the same way.
 */
struct SkyIrradiance
{
  std::array<glm::vec3, 9> coefficients{};

  glm::vec3 at(const glm::vec3& normal) const;

  bool operator==(const SkyIrradiance&) const = default;
};

/**
 * A physical sky: a single-scattering atmosphere (Rayleigh, Mie and ozone,
 * Earth's, after Hillaire 2020 without the LUTs it doesn't need here) with an
 * approximation of multiple scattering, lit by the sun. Evaluated on the CPU
 * (for the sun's colour and the sky's diffuse light) and, the same way, on
 * the GPU (sky.wgsl: the background and the reflections), from the
 * transmittance table built here.
 *
 * Light is in the units the lit shader shades in: sunLight() is what a
 * surface facing the sun receives (x albedo: its colour), about 1 at noon (so
 * a sunlit mid-grey shows as mid-grey through a tone mapper, with no
 * exposure); the sky's radiance is in the same units (about 0.1 for a blue
 * zenith, 0.5 at the horizon).
 * Directions are world space, Z up.
 */
class PhysicalSky
{
public:
  // The planet and the air (kilometres).
  static constexpr float kGroundRadius = 6360.0f;
  static constexpr float kTopRadius = 6460.0f;
  static constexpr float kViewerAltitude = 0.1f; // where everything's seen
  static constexpr float kRayleighHeight = 8.0f; // scale heights
  static constexpr float kMieHeight = 1.2f;
  static constexpr float kMieG = 0.8f; // how forward Mie scattering is
  // Light scattered more than once, as an isotropic share of what's scattered
  // once (Hillaire's multiple-scattering term, approximated).
  static constexpr float kMultipleScattering = 0.12f;
  // The sun's angular radius, radians.
  static constexpr float kSunRadius = 0.004654f;
  // How high the sun climbs (radians: 5 degrees) before nothing near the
  // horizon (low haze, hills, trees) holds its light back.
  static constexpr float kHorizonHaze = 0.0873f;
  // The most eyes adapt to the dark (3 stops): x8 the light they see.
  static constexpr float kMostAdaptation = 8.0f;
  // A point light's strength (as the eye sees it: x lampScale()) below which
  // its shadow doesn't show (none is drawn), and above which it shows whole.
  static constexpr float kLampShadowFrom = 0.2f;
  static constexpr float kLampShadowFull = 0.4f;
  // The transmittance table: altitude x the cosine of the zenith angle.
  static constexpr int kTransmittanceHeights = 32;
  static constexpr int kTransmittanceAngles = 128;
  // Samples along a view ray (the GPU takes as many).
  static constexpr int kViewSamples = 24;

  explicit PhysicalSky(const SkySettings& settings = SkySettings());

  const SkySettings& settings() const { return m_settings; }

  /** The sun's light at the top of the atmosphere (x pi: irradiance). */
  glm::vec3 solarIrradiance() const;

  /** Per-kilometre coefficients at sea level (rgb). */
  glm::vec3 rayleighScattering() const;
  float mieScattering() const;
  float mieExtinction() const;
  glm::vec3 ozoneAbsorption() const;

  /**
   * How much light gets through the air from `altitude` (km) along a
   * direction `cosZenith` from straight up, to space (0 if the ground's in
   * the way). From the table, interpolated.
   */
  glm::vec3 transmittance(float altitude, float cosZenith) const;

  /**
   * What a surface facing the sun receives from it (no sky), at the viewer:
   * its light through the air, faded in as it rises clear of the horizon
   * (clearOfHorizon()); none once it has set.
   */
  glm::vec3 sunLight(const glm::vec3& toSun) const;

  /**
   * How much of the sun's light the horizon lets through, 0..1: none until
   * its disk shows, all from kHorizonHaze up. (Its disk shows whole.)
   */
  static float clearOfHorizon(const glm::vec3& toSun);

  /**
   * What a surface facing the moon receives from it, at the viewer: a full
   * moon's light (bluish, under a percent of the sun's) x `share` (see
   * moonlight() in lighting.h), through the air.
   */
  glm::vec3 moonLight(const glm::vec3& toMoon, float share) const;

  /**
   * The sky's light from `direction` (no sun disk), seen from the viewer.
   * `night` scales its night glow (moonlight brightens it: see nightScale()).
   */
  glm::vec3 radiance(const glm::vec3& direction,
                     const glm::vec3& toSun,
                     float night = 1.0f) const;

  /**
   * The light the ground reflects (what's below the horizon): its albedo x
   * the sun's and the sky's light on it.
   */
  glm::vec3 groundRadiance(const glm::vec3& toSun, float night = 1.0f) const;

  /**
   * The night sky's glow from `direction` (what's left without the sun), x
   * `night`.
   */
  glm::vec3 nightGlow(const glm::vec3& direction, float night = 1.0f) const;

  /**
   * How much brighter than a moonless night's the night glow is under the
   * moon lighting the scene with `moonShare` (moonlight()).
   */
  static float nightScale(float moonShare);

  /** The sky's (and the ground's) diffuse light, all around. */
  SkyIrradiance irradiance(const glm::vec3& toSun, float night = 1.0f) const;

  /**
   * How much brighter eyes adapted to the light the ground gets (the sun's,
   * `sunLight` on it, and the sky's, `skyLight` from above) see it than at
   * noon, by the settings' adaptation: 1 by day, up to 8 at night.
   */
  float adaptation(const glm::vec3& toSun,
                   const glm::vec3& sunLight,
                   const glm::vec3& skyLight) const;

  /**
   * The adaptation() eyes reach in the deepest dark, by the settings: 1 if
   * they don't adapt, up to kMostAdaptation.
   */
  float mostAdaptation() const;

  /**
   * What lamps (point lights, and emission: their brightness is fixed, set to
   * look right to eyes adapted to the night) look like to eyes `adapted`
   * (adaptation()): x their share of mostAdaptation(), so 1 at night and down
   * to an eighth at noon, where daylight drowns all but strong ones out.
   */
  float lampScale(float adapted) const;

  /**
   * How much of a point light of `intensity` its shadow shows (0..1) to eyes
   * seeing lamps x `lampScale`: none (it needn't be drawn) when the light is
   * too dim to see, faded in (kLampShadowFrom to kLampShadowFull) as it
   * brightens. (meshLit.wgsl's pointShadow() fades it as this does.)
   */
  static float lampShadow(float intensity, float lampScale);

  /**
   * The transmittance table, rgba (a unused), row per altitude:
   * kTransmittanceAngles x kTransmittanceHeights texels (see
   * transmittanceUv() for where a lookup lands).
   */
  const std::vector<glm::vec4>& transmittanceTable() const
  {
    return m_transmittance;
  }

  /** Where (altitude, cosZenith) lands in the table, 0..1 each. */
  static glm::vec2 transmittanceUv(float altitude, float cosZenith);

private:
  glm::vec3 opticalDepth(float altitude, float cosZenith) const;
  glm::vec3 skyRadiance(const glm::vec3& direction,
                        const glm::vec3& toSun,
                        const glm::vec3& ground,
                        float night) const;

  SkySettings m_settings;
  std::vector<glm::vec4> m_transmittance;
};

/**
 * The split-sum BRDF table image-based lighting needs (Karis 2013): for a
 * surface's n.v (x, 0..1) and roughness (y, 0..1), the scale (r) and bias (g)
 * on its reflectance face-on (f0) that integrate GGX specular over the
 * environment: reflected = prefiltered x (f0 x r + g). `size` x `size`
 * texels, row per roughness; worked out on the CPU (importance-sampled).
 */
std::vector<glm::vec2> brdfTable(int size);

} // namespace sfs
