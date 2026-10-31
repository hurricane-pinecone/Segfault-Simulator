#include "engine/core/rendering/sky.h"

#include "glm/glm/common.hpp"
#include "glm/glm/exponential.hpp"
#include "glm/glm/ext/scalar_constants.hpp"
#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>

namespace sfs
{

namespace
{

constexpr float kPi = glm::pi<float>();

// Earth's air (per kilometre at sea level; Hillaire 2020's).
constexpr glm::vec3 kRayleigh{5.802e-3f, 13.558e-3f, 33.1e-3f};
constexpr float kMieScattering = 3.996e-3f; // at turbidity 2
constexpr float kMieExtinction = 4.440e-3f;
constexpr glm::vec3 kOzone{0.650e-3f, 1.881e-3f, 0.085e-3f};
// The table's highest altitude.
constexpr float kTableTop =
    PhysicalSky::kTopRadius - PhysicalSky::kGroundRadius;
// The sun's light at the top of the air, as a surface facing it receives it
// (what sunLight() is before the air takes its share): about 1 at noon, so a
// sunlit mid-grey lands near mid-grey with no exposure.
constexpr float kSunLight = 1.05f;
// The night sky's glow (rgb) at the zenith.
constexpr float kRayleighHeight = PhysicalSky::kRayleighHeight;
constexpr float kMieHeight = PhysicalSky::kMieHeight;
constexpr float kMieG = PhysicalSky::kMieG;
constexpr float kMultipleScattering = PhysicalSky::kMultipleScattering;
constexpr float kSunRadius = PhysicalSky::kSunRadius;
constexpr glm::vec3 kNightGlow{0.0018f, 0.0030f, 0.0075f};
// A full moon's light on a surface facing it, above the air: more than the
// real moon's (a millionth of the sun's), but only enough that, once eyes
// adapt, a moonlit landscape is a little lighter and bluer than a moonless
// one.
constexpr glm::vec3 kMoonLight{0.0045f, 0.0058f, 0.0090f};
// How much a full moon high in a clear sky brightens the night glow: hardly
// (the night sky stays black).
constexpr float kMoonSkyGlow = 0.1f;

float ozoneDensity(float altitude)
{
  return std::max(0.0f, 1.0f - std::abs(altitude - 25.0f) / 15.0f);
}

// Distance from radius `r` along `cosZenith` to the top of the air.
float distanceToTop(float r, float cosZenith)
{
  const float top = PhysicalSky::kTopRadius;
  const float disc = r * r * (cosZenith * cosZenith - 1.0f) + top * top;
  return std::max(0.0f, -r * cosZenith + std::sqrt(std::max(disc, 0.0f)));
}

// Distance to the ground along `cosZenith`, or a negative number if the ray
// misses it.
float distanceToGround(float r, float cosZenith)
{
  const float ground = PhysicalSky::kGroundRadius;
  const float disc = r * r * (cosZenith * cosZenith - 1.0f) + ground * ground;
  if (cosZenith >= 0.0f || disc < 0.0f)
    return -1.0f;
  return std::max(0.0f, -r * cosZenith - std::sqrt(disc));
}

float rayleighPhase(float cosAngle)
{
  return 3.0f / (16.0f * kPi) * (1.0f + cosAngle * cosAngle);
}

// Cornette-Shanks.
float miePhase(float cosAngle)
{
  const float g = kMieG;
  const float g2 = g * g;
  const float denominator =
      std::pow(std::max(1.0f + g2 - 2.0f * g * cosAngle, 1e-4f), 1.5f);
  return 3.0f / (8.0f * kPi) * (1.0f - g2) * (1.0f + cosAngle * cosAngle) /
         ((2.0f + g2) * denominator);
}

// The real spherical harmonics' first nine, for a unit direction.
std::array<float, 9> shBasis(const glm::vec3& d)
{
  return {0.282095f,
          0.488603f * d.y,
          0.488603f * d.z,
          0.488603f * d.x,
          1.092548f * d.x * d.y,
          1.092548f * d.y * d.z,
          0.315392f * (3.0f * d.z * d.z - 1.0f),
          1.092548f * d.x * d.z,
          0.546274f * (d.x * d.x - d.y * d.y)};
}

} // namespace

const char* skyModelName(SkyModel model)
{
  return model == SkyModel::Physical ? "physical" : "flat";
}

std::optional<SkyModel> skyModelFromName(std::string_view name)
{
  if (name == "flat")
    return SkyModel::Flat;
  if (name == "physical")
    return SkyModel::Physical;
  return std::nullopt;
}

nlohmann::json SkySettings::toJson() const
{
  return {{"model", skyModelName(model)},
          {"turbidity", turbidity},
          {"groundAlbedo", groundAlbedo},
          {"sunSize", sunSize},
          {"intensity", intensity},
          {"nightBrightness", nightBrightness},
          {"adaptation", adaptation}};
}

SkySettings SkySettings::fromJson(const nlohmann::json& json,
                                  const SkySettings& defaults)
{
  SkySettings out = defaults;
  if (!json.is_object())
    return out;
  if (const auto it = json.find("model"); it != json.end() && it->is_string())
    out.model = skyModelFromName(it->get<std::string>()).value_or(out.model);
  const auto read = [&](const char* key, float& value)
  {
    if (const auto it = json.find(key); it != json.end() && it->is_number())
      value = it->get<float>();
  };
  read("turbidity", out.turbidity);
  read("groundAlbedo", out.groundAlbedo);
  read("sunSize", out.sunSize);
  read("intensity", out.intensity);
  read("nightBrightness", out.nightBrightness);
  read("adaptation", out.adaptation);
  return out;
}

SkySettings SkySettings::fromJson(const nlohmann::json& json)
{
  return fromJson(json, SkySettings());
}

glm::vec3 SkyIrradiance::at(const glm::vec3& normal) const
{
  const std::array<float, 9> basis = shBasis(normal);
  glm::vec3 sum(0.0f);
  for (std::size_t i = 0; i < basis.size(); ++i)
    sum += coefficients[i] * basis[i];
  return glm::max(sum, glm::vec3(0.0f));
}

PhysicalSky::PhysicalSky(const SkySettings& settings) : m_settings(settings)
{
  m_settings.turbidity = std::clamp(m_settings.turbidity, 0.5f, 20.0f);
  m_settings.groundAlbedo = std::clamp(m_settings.groundAlbedo, 0.0f, 1.0f);
  m_settings.intensity = std::max(m_settings.intensity, 0.0f);
  m_settings.nightBrightness = std::max(m_settings.nightBrightness, 0.0f);
  m_settings.adaptation = std::clamp(m_settings.adaptation, 0.0f, 1.0f);

  m_transmittance.resize(std::size_t(kTransmittanceAngles) *
                         kTransmittanceHeights);
  for (int y = 0; y < kTransmittanceHeights; ++y)
  {
    const float v = static_cast<float>(y) / (kTransmittanceHeights - 1);
    const float altitude = v * v * kTableTop;
    for (int x = 0; x < kTransmittanceAngles; ++x)
    {
      const float s =
          2.0f * static_cast<float>(x) / (kTransmittanceAngles - 1) - 1.0f;
      const float cosZenith = s * std::abs(s);
      const glm::vec3 depth = opticalDepth(altitude, cosZenith);
      m_transmittance[std::size_t(y) * kTransmittanceAngles + x] =
          glm::vec4(glm::exp(-depth), 1.0f);
    }
  }
}

glm::vec3 PhysicalSky::solarIrradiance() const
{
  return glm::vec3(kSunLight * kPi * m_settings.intensity);
}

glm::vec3 PhysicalSky::rayleighScattering() const { return kRayleigh; }

float PhysicalSky::mieScattering() const
{
  return kMieScattering * m_settings.turbidity * 0.5f;
}

float PhysicalSky::mieExtinction() const
{
  return kMieExtinction * m_settings.turbidity * 0.5f;
}

glm::vec3 PhysicalSky::ozoneAbsorption() const { return kOzone; }

glm::vec3 PhysicalSky::opticalDepth(float altitude, float cosZenith) const
{
  const float r = kGroundRadius + altitude;
  if (distanceToGround(r, cosZenith) >= 0.0f)
    return glm::vec3(1e4f); // the ground's in the way
  const float length = distanceToTop(r, cosZenith);
  constexpr int kSteps = 40;
  const float step = length / kSteps;
  float rayleigh = 0.0f;
  float mie = 0.0f;
  float ozone = 0.0f;
  for (int i = 0; i < kSteps; ++i)
  {
    const float t = (static_cast<float>(i) + 0.5f) * step;
    const float h =
        std::sqrt(r * r + t * t + 2.0f * r * cosZenith * t) - kGroundRadius;
    rayleigh += std::exp(-h / kRayleighHeight);
    mie += std::exp(-h / kMieHeight);
    ozone += ozoneDensity(h);
  }
  return (kRayleigh * rayleigh + glm::vec3(mieExtinction() * mie) +
          kOzone * ozone) *
         step;
}

glm::vec2 PhysicalSky::transmittanceUv(float altitude, float cosZenith)
{
  const float v = std::sqrt(std::clamp(altitude / kTableTop, 0.0f, 1.0f));
  const float c = std::clamp(cosZenith, -1.0f, 1.0f);
  const float u = 0.5f * (std::copysign(std::sqrt(std::abs(c)), c) + 1.0f);
  // Onto texel centres.
  return {(u * (kTransmittanceAngles - 1) + 0.5f) / kTransmittanceAngles,
          (v * (kTransmittanceHeights - 1) + 0.5f) / kTransmittanceHeights};
}

glm::vec3 PhysicalSky::transmittance(float altitude, float cosZenith) const
{
  const glm::vec2 uv = transmittanceUv(altitude, cosZenith);
  const float x = std::clamp(uv.x * kTransmittanceAngles - 0.5f,
                             0.0f,
                             static_cast<float>(kTransmittanceAngles - 1));
  const float y = std::clamp(uv.y * kTransmittanceHeights - 0.5f,
                             0.0f,
                             static_cast<float>(kTransmittanceHeights - 1));
  const int x0 = std::min(static_cast<int>(x), kTransmittanceAngles - 2);
  const int y0 = std::min(static_cast<int>(y), kTransmittanceHeights - 2);
  const float fx = x - static_cast<float>(x0);
  const float fy = y - static_cast<float>(y0);
  const auto at = [&](int ix, int iy)
  {
    return glm::vec3(
        m_transmittance[std::size_t(iy) * kTransmittanceAngles + ix]);
  };
  return glm::mix(glm::mix(at(x0, y0), at(x0 + 1, y0), fx),
                  glm::mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx),
                  fy);
}

glm::vec3 PhysicalSky::sunLight(const glm::vec3& toSun) const
{
  const float length = glm::length(toSun);
  if (length < 1e-6f)
    return glm::vec3(0.0f);
  const glm::vec3 sun = toSun / length;
  return glm::vec3(kSunLight * m_settings.intensity) *
         transmittance(kViewerAltitude, std::max(sun.z, 0.0f)) *
         clearOfHorizon(sun);
}

float PhysicalSky::clearOfHorizon(const glm::vec3& toSun)
{
  // From its disk's first showing to kHorizonHaze up; so shadows fade in
  // as it rises, rather than snapping on (a day/night cycle can lift it
  // through its own width in a fraction of a second).
  const float length = glm::length(toSun);
  if (length < 1e-6f)
    return 0.0f;
  const float elevation = std::asin(std::clamp(toSun.z / length, -1.0f, 1.0f));
  return glm::smoothstep(-kSunRadius, kHorizonHaze, elevation);
}

glm::vec3 PhysicalSky::moonLight(const glm::vec3& toMoon, float share) const
{
  const glm::vec3 moon = glm::normalize(toMoon);
  return kMoonLight * (m_settings.intensity * std::max(share, 0.0f)) *
         transmittance(kViewerAltitude, std::max(moon.z, 0.0f));
}

float PhysicalSky::nightScale(float moonShare)
{
  return 1.0f + kMoonSkyGlow * std::max(moonShare, 0.0f);
}

glm::vec3 PhysicalSky::nightGlow(const glm::vec3& direction, float night) const
{
  // Brighter toward the horizon (airglow), a little.
  const float up = std::clamp(direction.z, -1.0f, 1.0f);
  const float horizon = 1.0f - std::abs(up);
  return kNightGlow * (1.0f + 1.5f * horizon * horizon * horizon) *
         (m_settings.nightBrightness * m_settings.intensity * night);
}

glm::vec3 PhysicalSky::skyRadiance(const glm::vec3& direction,
                                   const glm::vec3& toSun,
                                   const glm::vec3& ground,
                                   float night) const
{
  const glm::vec3 dir = glm::normalize(direction);
  const glm::vec3 sun = glm::normalize(toSun);
  const float r0 = kGroundRadius + kViewerAltitude;
  const float mu = dir.z;
  const float toGround = distanceToGround(r0, mu);
  const bool hitsGround = toGround >= 0.0f;
  const float end = hitsGround ? toGround : distanceToTop(r0, mu);

  const float nu = glm::dot(dir, sun);
  const float phaseR = rayleighPhase(nu);
  const float phaseM = miePhase(nu);
  const float mieScatter = mieScattering();
  const float mieExtinct = mieExtinction();

  glm::vec3 depth(0.0f); // optical depth from the viewer
  glm::vec3 light(0.0f);
  float previous = 0.0f;
  for (int i = 0; i < kViewSamples; ++i)
  {
    // Denser near the viewer, where the air is.
    const float f = static_cast<float>(i + 1) / kViewSamples;
    const float next = end * f * f;
    const float dt = next - previous;
    const float t = 0.5f * (previous + next);
    previous = next;

    const glm::vec3 position = glm::vec3(0.0f, 0.0f, r0) + dir * t;
    const float r = glm::length(position);
    const float h = std::max(r - kGroundRadius, 0.0f);
    const float sunCos = glm::dot(position / r, sun);
    const float densityR = std::exp(-h / kRayleighHeight);
    const float densityM = std::exp(-h / kMieHeight);
    const glm::vec3 extinction = kRayleigh * densityR +
                                 glm::vec3(mieExtinct * densityM) +
                                 kOzone * ozoneDensity(h);

    const glm::vec3 throughView = glm::exp(-(depth + extinction * (0.5f * dt)));
    depth += extinction * dt;
    const glm::vec3 throughSun = transmittance(h, sunCos);
    const glm::vec3 scatterR = kRayleigh * densityR;
    const glm::vec3 scatterM(mieScatter * densityM);
    const glm::vec3 single = scatterR * phaseR + scatterM * phaseM;
    const glm::vec3 multiple = (scatterR + scatterM) * kMultipleScattering;
    light += throughView * throughSun * (single + multiple) * dt;
  }
  light *= solarIrradiance();
  if (hitsGround)
    light += glm::exp(-depth) * ground;
  return light + nightGlow(dir, night);
}

glm::vec3 PhysicalSky::radiance(const glm::vec3& direction,
                                const glm::vec3& toSun,
                                float night) const
{
  return skyRadiance(direction, toSun, groundRadiance(toSun, night), night);
}

glm::vec3 PhysicalSky::groundRadiance(const glm::vec3& toSun, float night) const
{
  // The sky's light on the ground: its upper half, cosine-weighted (/ pi).
  const glm::vec3 sun = glm::normalize(toSun);
  constexpr int kRings = 4;
  constexpr int kSegments = 8;
  glm::vec3 sky(0.0f);
  for (int i = 0; i < kRings; ++i)
  {
    const float theta = (static_cast<float>(i) + 0.5f) / kRings * kPi * 0.5f;
    const float weight = std::cos(theta) * std::sin(theta) *
                         (kPi * 0.5f / kRings) * (2.0f * kPi / kSegments);
    for (int j = 0; j < kSegments; ++j)
    {
      const float phi = (static_cast<float>(j) + 0.5f) / kSegments * 2.0f * kPi;
      const glm::vec3 dir(std::sin(theta) * std::cos(phi),
                          std::sin(theta) * std::sin(phi),
                          std::cos(theta));
      sky += skyRadiance(dir, sun, glm::vec3(0.0f), night) * weight;
    }
  }
  sky /= kPi;
  const glm::vec3 direct = sunLight(sun) * std::max(sun.z, 0.0f);
  return m_settings.groundAlbedo * (direct + sky);
}

float PhysicalSky::adaptation(const glm::vec3& toSun,
                              const glm::vec3& sunLight,
                              const glm::vec3& skyLight) const
{
  // The light on the ground against noon's (about 0.9 x intensity), adapted
  // to by a share of the stops between, at most 3.
  const glm::vec3 sun = glm::normalize(toSun);
  const glm::vec3 ground = sunLight * std::max(sun.z, 0.0f) + skyLight;
  const float light =
      0.2126f * ground.r + 0.7152f * ground.g + 0.0722f * ground.b;
  const float noon = 0.9f * std::max(m_settings.intensity, 1e-3f);
  const float stops =
      std::log2(noon / std::max(light, 1e-6f)) * m_settings.adaptation;
  return std::exp2(std::clamp(stops, 0.0f, 3.0f));
}

SkyIrradiance PhysicalSky::irradiance(const glm::vec3& toSun, float night) const
{
  const glm::vec3 sun = glm::normalize(toSun);
  const glm::vec3 ground = groundRadiance(sun, night);
  // The light from all around, projected onto the harmonics.
  constexpr int kRings = 10;
  constexpr int kSegments = 16;
  std::array<glm::vec3, 9> light{};
  for (int i = 0; i < kRings; ++i)
  {
    const float theta = (static_cast<float>(i) + 0.5f) / kRings * kPi;
    const float weight =
        std::sin(theta) * (kPi / kRings) * (2.0f * kPi / kSegments);
    for (int j = 0; j < kSegments; ++j)
    {
      const float phi = (static_cast<float>(j) + 0.5f) / kSegments * 2.0f * kPi;
      const glm::vec3 dir(std::sin(theta) * std::cos(phi),
                          std::sin(theta) * std::sin(phi),
                          std::cos(theta));
      const glm::vec3 radiance = skyRadiance(dir, sun, ground, night);
      const std::array<float, 9> basis = shBasis(dir);
      for (std::size_t k = 0; k < basis.size(); ++k)
        light[k] += radiance * (basis[k] * weight);
    }
  }
  // Convolved with the cosine lobe, / pi (Ramamoorthi & Hanrahan).
  constexpr std::array<float, 9> band = {1.0f,
                                         2.0f / 3.0f,
                                         2.0f / 3.0f,
                                         2.0f / 3.0f,
                                         0.25f,
                                         0.25f,
                                         0.25f,
                                         0.25f,
                                         0.25f};
  SkyIrradiance out;
  for (std::size_t k = 0; k < band.size(); ++k)
    out.coefficients[k] = light[k] * band[k];
  return out;
}

std::vector<glm::vec2> brdfTable(int size)
{
  size = std::max(size, 2);
  constexpr uint32_t kSamples = 256;
  std::vector<glm::vec2> out(std::size_t(size) * size);
  for (int y = 0; y < size; ++y)
  {
    const float roughness =
        std::max((static_cast<float>(y) + 0.5f) / size, 0.02f);
    const float a = roughness * roughness;
    const float k = a * 0.5f; // Schlick-GGX for image-based light
    for (int x = 0; x < size; ++x)
    {
      const float ndotv =
          std::max((static_cast<float>(x) + 0.5f) / size, 1e-3f);
      const glm::vec3 view(std::sqrt(1.0f - ndotv * ndotv), 0.0f, ndotv);
      float scale = 0.0f;
      float bias = 0.0f;
      for (uint32_t i = 0; i < kSamples; ++i)
      {
        // Hammersley, then GGX's distribution of half vectors.
        uint32_t bits = i;
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        const float u = static_cast<float>(i) / kSamples;
        const float v = static_cast<float>(bits) * 2.3283064365386963e-10f;
        const float phi = 2.0f * kPi * u;
        const float cosTheta =
            std::sqrt((1.0f - v) / (1.0f + (a * a - 1.0f) * v));
        const float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);
        const glm::vec3 half(
            sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
        const glm::vec3 light = 2.0f * glm::dot(view, half) * half - view;
        const float ndotl = light.z;
        if (ndotl <= 0.0f)
          continue;
        const float ndoth = std::max(half.z, 0.0f);
        const float vdoth = std::max(glm::dot(view, half), 0.0f);
        const float g = (ndotl / (ndotl * (1.0f - k) + k)) *
                        (ndotv / (ndotv * (1.0f - k) + k));
        const float visibility = g * vdoth / std::max(ndoth * ndotv, 1e-6f);
        const float fresnel = std::pow(1.0f - vdoth, 5.0f);
        scale += (1.0f - fresnel) * visibility;
        bias += fresnel * visibility;
      }
      out[std::size_t(y) * size + x] =
          glm::vec2(scale, bias) / static_cast<float>(kSamples);
    }
  }
  return out;
}

} // namespace sfs
