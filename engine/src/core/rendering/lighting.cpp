#include "engine/core/rendering/lighting.h"

#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"

#include <algorithm>
#include <cmath>

namespace sfs
{

float clearSky(float cloudCover)
{
  // Broken cloud lets the sun through its gaps; a closed deck (the last
  // quarter of the cover) hardly any: an overcast day casts no shadows.
  // (meshLit.wgsl's cloudShadowAt closes the deck the same way.)
  const float cover = glm::clamp(cloudCover, 0.0f, 1.0f);
  const float deck = glm::smoothstep(0.75f, 1.0f, cover);
  return (1.0f - 0.85f * cover) * (1.0f - 0.9f * deck);
}

float moonlight(const SceneLighting& lighting)
{
  const glm::vec3& moon = lighting.moon.direction;
  const glm::vec3& sun = lighting.sun.direction;
  if (glm::dot(moon, moon) < 1e-12f)
    return 0.0f;
  const float moonUp = glm::normalize(moon).z;
  const float sunUp =
      glm::dot(sun, sun) > 1e-12f ? glm::normalize(sun).z : -1.0f;
  // A half moon gives far less than half a full one's light.
  const float phase = glm::clamp(lighting.moon.phase, 0.0f, 1.0f);
  const float risen = glm::smoothstep(0.0f, 0.15f, moonUp);
  const float dark = 1.0f - glm::smoothstep(-0.10f, -0.005f, sunUp);
  return phase * phase * risen * dark * clearSky(lighting.cloudCover);
}

void setSpot(PointLight& light,
             glm::vec3 direction,
             float innerDegrees,
             float outerDegrees)
{
  const float length = glm::length(direction);
  if (!(length > 1e-6f))
  {
    light.spotDirection = glm::vec3(0.0f);
    light.spotCosInner = light.spotCosOuter = -1.0f;
    return;
  }
  const float inner = std::clamp(innerDegrees, 0.0f, 180.0f);
  const float outer = std::clamp(outerDegrees, inner, 180.0f);
  light.spotDirection = direction / length;
  light.spotCosInner = std::cos(glm::radians(inner));
  light.spotCosOuter = std::cos(glm::radians(outer));
}

glm::vec4 lightBounds(const PointLight& light)
{
  const float reach = std::max(light.radius, 0.0f);
  const float cosine = std::clamp(light.spotCosOuter, -1.0f, 1.0f);
  // A cone with a round cap (all of it within `reach` of its tip), opening
  // `angle` either side of its axis: up to 45 degrees, the sphere through its
  // tip and its cap's rim; wider, the one round its rim (out to 90; past
  // that, the light's own sphere is as small).
  if (!light.spot() || cosine <= 0.0f)
    return glm::vec4(light.position, reach);
  constexpr float kCos45 = 0.70710678f;
  if (cosine >= kCos45)
  {
    const float r = reach / (2.0f * cosine);
    return glm::vec4(light.position + light.spotDirection * r, r);
  }
  const float sine = std::sqrt(std::max(1.0f - cosine * cosine, 0.0f));
  return glm::vec4(
      light.position + light.spotDirection * (reach * cosine), reach * sine);
}

bool lightReaches(const PointLight& light, glm::vec3 center, float radius)
{
  const glm::vec3 offset = center - light.position;
  const float distanceSq = glm::dot(offset, offset);
  const float reach = light.radius + radius;
  if (distanceSq > reach * reach)
    return false;
  if (!light.spot() || light.spotCosOuter <= -1.0f)
    return true;
  // The sphere against the cone (its tip, its axis, opening `angle`): along
  // the axis, and how far outside the cone's side its centre is.
  const float along = glm::dot(offset, light.spotDirection);
  const float across = std::sqrt(std::max(distanceSq - along * along, 0.0f));
  const float cosine = std::clamp(light.spotCosOuter, -1.0f, 1.0f);
  const float sine = std::sqrt(std::max(1.0f - cosine * cosine, 0.0f));
  const float outside = across * cosine - along * sine;
  return outside <= radius;
}

bool lightReachesFace(const PointLight& light, int face)
{
  if (!light.spot())
    return true;
  // A face's view is a 90 degree pyramid round its axis: its corners are
  // acos(1 / sqrt 3) (54.7 degrees) from it. The cone reaches into it if its
  // axis is within that plus its own angle of the face's axis.
  glm::vec3 axis(0.0f);
  axis[face / 2] = face % 2 == 0 ? 1.0f : -1.0f;
  const float between =
      std::acos(std::clamp(glm::dot(axis, light.spotDirection), -1.0f, 1.0f));
  const float opening = std::acos(std::clamp(light.spotCosOuter, -1.0f, 1.0f));
  constexpr float kFaceCorner = 0.95531662f; // acos(1 / sqrt(3))
  return between <= kFaceCorner + opening + 1e-4f;
}

} // namespace sfs
