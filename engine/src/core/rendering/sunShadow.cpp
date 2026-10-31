#include "engine/core/rendering/sunShadow.h"

#include "glm/glm/ext/matrix_clip_space.hpp"
#include "glm/glm/ext/matrix_transform.hpp"
#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sfs
{

glm::mat4 fitSunShadow(const glm::vec3& sunDirection,
                       const glm::vec3& center,
                       float radius,
                       uint32_t resolution)
{
  const glm::vec3 toSun = glm::normalize(sunDirection);

  // lookAt needs an up vector that isn't parallel to the view; near noon the
  // sun is straight overhead, so fall back to +Y.
  const glm::vec3 up = std::abs(toSun.z) > 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                 : glm::vec3(0.0f, 0.0f, 1.0f);

  // Sit the light outside the covered sphere so every caster in it lies
  // between the near and far planes.
  const glm::vec3 eye = center + toSun * (radius * 2.0f);
  const glm::mat4 view = glm::lookAtRH(eye, center, up);
  glm::mat4 projection =
      glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, radius * 4.0f);

  // Snap: move the projection so the world origin lands on a texel corner.
  // The covered region then only ever shifts by whole texels.
  const glm::mat4 viewProjection = projection * view;
  const float texelsPerUnit =
      static_cast<float>(std::max(resolution, 1u)) * 0.5f;
  const glm::vec4 origin = viewProjection * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  const glm::vec2 originTexels = glm::vec2(origin) * texelsPerUnit;
  const glm::vec2 offset =
      (glm::vec2(std::round(originTexels.x), std::round(originTexels.y)) -
       originTexels) /
      texelsPerUnit;

  projection[3][0] += offset.x;
  projection[3][1] += offset.y;
  return projection * view;
}

std::array<float, kMaxShadowCascades + 1>
cascadeSplits(uint32_t count, float nearPlane, float farPlane, float lambda)
{
  count = std::clamp(count, 1u, kMaxShadowCascades);
  const float n = std::max(nearPlane, 1e-3f);
  const float f = std::max(farPlane, n * 1.001f);
  const float blend = std::clamp(lambda, 0.0f, 1.0f);
  std::array<float, kMaxShadowCascades + 1> splits{};
  splits.fill(f);
  splits[0] = n;
  for (uint32_t i = 1; i < count; ++i)
  {
    const float part = static_cast<float>(i) / static_cast<float>(count);
    const float logarithmic = n * std::pow(f / n, part);
    const float even = n + (f - n) * part;
    splits[i] = blend * logarithmic + (1.0f - blend) * even;
  }
  return splits;
}

glm::vec2
cascadeSphere(const CascadeView& view, float splitNear, float splitFar)
{
  const float n = splitNear;
  const float f = std::max(splitFar, n);
  if (!view.perspective)
  {
    const float half = 0.5f * (f - n);
    return {n + half,
            std::sqrt(view.halfExtent.x * view.halfExtent.x +
                      view.halfExtent.y * view.halfExtent.y + half * half)};
  }
  // A corner at distance d along the view is d * t off its axis. The sphere
  // through the near and far corners has its centre where they're equally
  // far; past the far plane, the far corners' circle bounds it all.
  const float t2 = view.halfExtent.x * view.halfExtent.x +
                   view.halfExtent.y * view.halfExtent.y;
  const float center = std::min(0.5f * (f + n) * (1.0f + t2), f);
  const float radius =
      std::max(std::sqrt((f - center) * (f - center) + f * f * t2),
               std::sqrt((center - n) * (center - n) + n * n * t2));
  return {center, radius};
}

SunCascade fitSunCascade(const glm::vec3& sunDirection,
                         const glm::vec3& center,
                         float radius,
                         float casterReach,
                         uint32_t resolution)
{
  const glm::vec3 toSun = glm::normalize(sunDirection);
  const glm::vec3 up = std::abs(toSun.z) > 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                 : glm::vec3(0.0f, 0.0f, 1.0f);
  const float reach = std::max(casterReach, radius);
  const glm::vec3 eye = center + toSun * (radius + reach);
  const float depth = 2.0f * radius + reach;
  const glm::mat4 view = glm::lookAtRH(eye, center, up);
  glm::mat4 projection =
      glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, depth);

  // Snapped as fitSunShadow's: the world origin on a texel corner.
  const glm::mat4 viewProjection = projection * view;
  const float texelsPerUnit =
      static_cast<float>(std::max(resolution, 1u)) * 0.5f;
  const glm::vec4 origin = viewProjection * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  const glm::vec2 originTexels = glm::vec2(origin) * texelsPerUnit;
  const glm::vec2 offset =
      (glm::vec2(std::round(originTexels.x), std::round(originTexels.y)) -
       originTexels) /
      texelsPerUnit;
  projection[3][0] += offset.x;
  projection[3][1] += offset.y;

  SunCascade cascade;
  cascade.viewProjection = projection * view;
  cascade.center = center;
  cascade.radius = radius;
  cascade.depthRange = depth;
  return cascade;
}

SunCascades fitSunCascades(const glm::vec3& sunDirection,
                           const CascadeView& view,
                           uint32_t count,
                           float maxDistance,
                           float lambda,
                           uint32_t resolution)
{
  SunCascades out;
  out.count = std::clamp(count, 1u, kMaxShadowCascades);
  const float farPlane =
      std::min(std::max(maxDistance, view.nearPlane * 2.0f), view.farPlane);
  const std::array<float, kMaxShadowCascades + 1> splits =
      cascadeSplits(out.count, view.nearPlane, farPlane, lambda);
  // Casters as far toward the sun as the shadows reach still cast into the
  // nearest cascades.
  const float reach = std::max(farPlane * 0.5f, 20.0f);
  for (uint32_t i = 0; i < out.count; ++i)
  {
    const glm::vec2 sphere = cascadeSphere(view, splits[i], splits[i + 1]);
    // Its radius rounded up a little, so float noise never changes it (nor
    // the texel size).
    const float radius = std::ceil(sphere.y * 16.0f) / 16.0f;
    SunCascade& cascade = out.cascades[i];
    cascade = fitSunCascade(sunDirection,
                            view.position + view.forward * sphere.x,
                            radius,
                            reach,
                            resolution);
    cascade.splitNear = splits[i];
    cascade.splitFar = splits[i + 1];
  }
  return out;
}

glm::mat4 cascadeBounds(const glm::vec3& sunDirection,
                        const SunCascades& cascades)
{
  if (cascades.count == 0)
    return glm::mat4(1.0f);
  const glm::vec3 toSun = glm::normalize(sunDirection);
  const glm::vec3 up = std::abs(toSun.z) > 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                 : glm::vec3(0.0f, 0.0f, 1.0f);
  // In the light's view from the last cascade's eye: each box's extent.
  const SunCascade& last = cascades.cascades[cascades.count - 1];
  const glm::vec3 eye = last.center + toSun * (last.depthRange - last.radius);
  const glm::mat4 view = glm::lookAtRH(eye, last.center, up);
  glm::vec2 low(std::numeric_limits<float>::max());
  glm::vec2 high(std::numeric_limits<float>::lowest());
  float nearest = std::numeric_limits<float>::max();
  float farthest = std::numeric_limits<float>::lowest();
  for (uint32_t i = 0; i < cascades.count; ++i)
  {
    const SunCascade& cascade = cascades.cascades[i];
    const glm::vec3 center(view * glm::vec4(cascade.center, 1.0f));
    low = glm::min(low, glm::vec2(center) - cascade.radius);
    high = glm::max(high, glm::vec2(center) + cascade.radius);
    // Its eye is depthRange - radius before its centre; its far side,
    // radius past it (the view looks down -z).
    nearest =
        std::min(nearest, -center.z - (cascade.depthRange - cascade.radius));
    farthest = std::max(farthest, -center.z + cascade.radius);
  }
  return glm::orthoRH_ZO(low.x, high.x, low.y, high.y, nearest, farthest) *
         view;
}

} // namespace sfs
