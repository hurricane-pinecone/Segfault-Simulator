#pragma once

#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <array>
#include <cstdint>

namespace sfs
{

/**
 * Light view-projection for a directional shadow map covering the sphere of
 * `radius` around `center`, seen from `sunDirection` (pointing toward the
 * sun). Depth maps to [0, 1]. A sphere rather than a box, so the covered
 * region is the same whichever way the sun points.
 *
 * The projection is snapped to whole shadow-map texels, so shadow edges stay
 * put while the covered region follows a moving camera instead of crawling.
 *
 * @param resolution shadow-map edge in texels (for the snapping)
 */
glm::mat4 fitSunShadow(const glm::vec3& sunDirection,
                       const glm::vec3& center,
                       float radius,
                       uint32_t resolution);

/** Most sun shadow cascades (ShadowSettings::cascades is capped to it). */
inline constexpr uint32_t kMaxShadowCascades = 4;

/** What fitting the sun's shadow cascades needs of the camera. */
struct CascadeView
{
  glm::vec3 position{0.0f};
  glm::vec3 forward{0.0f, 1.0f, 0.0f}; // unit
  bool perspective = true;
  // Perspective: the tangents of half the view's angle across and up;
  // orthographic: half the view's width and height, world units.
  glm::vec2 halfExtent{1.0f, 1.0f};
  float nearPlane = 0.1f;
  float farPlane = 500.0f;
};

/**
 * Where the camera's view is split between cascades: `count` + 1 distances
 * along it, from `nearPlane` to `farPlane`, by the practical split scheme
 * (Zhang et al.): `lambda` 1 spaces them logarithmically (detail near the
 * camera), 0 evenly, between a blend. Unused entries are `farPlane`.
 */
std::array<float, kMaxShadowCascades + 1>
cascadeSplits(uint32_t count, float nearPlane, float farPlane, float lambda);

/**
 * The smallest sphere around the part of the view between `splitNear` and
 * `splitFar` (along it): its centre's distance along the view, and its
 * radius. The radius depends only on the view's shape, never which way it
 * looks, so a cascade fitted to it keeps its size (and texel size) as the
 * camera turns.
 */
glm::vec2
cascadeSphere(const CascadeView& view, float splitNear, float splitFar);

/** One sun shadow cascade, fitted to a slice of the view. */
struct SunCascade
{
  glm::mat4 viewProjection{1.0f};
  glm::vec3 center{0.0f}; // the sphere it covers
  float radius = 0.0f;
  float depthRange = 0.0f; // the world distance its depth 0..1 spans
  float splitNear = 0.0f;  // the slice of the view it's for
  float splitFar = 0.0f;
};

/** A frame's cascades (count 0: none, the single shadow map as ever). */
struct SunCascades
{
  uint32_t count = 0;
  std::array<SunCascade, kMaxShadowCascades> cascades{};
};

/**
 * Light view-projection for one cascade: the sphere of `radius` around
 * `center`, as fitSunShadow (snapped to whole texels, so its shadows don't
 * shimmer as the camera moves), but reaching `casterReach` further toward the
 * sun, so things that far beyond it still cast into it.
 */
SunCascade fitSunCascade(const glm::vec3& sunDirection,
                         const glm::vec3& center,
                         float radius,
                         float casterReach,
                         uint32_t resolution);

/**
 * `count` cascades (at most kMaxShadowCascades) over the view up to
 * `maxDistance` (or its far plane, if nearer), split by cascadeSplits().
 */
SunCascades fitSunCascades(const glm::vec3& sunDirection,
                           const CascadeView& view,
                           uint32_t count,
                           float maxDistance,
                           float lambda,
                           uint32_t resolution);

/**
 * One view-projection from the sun covering every cascade's box (for culling
 * what casts into any of them).
 */
glm::mat4 cascadeBounds(const glm::vec3& sunDirection,
                        const SunCascades& cascades);

} // namespace sfs
