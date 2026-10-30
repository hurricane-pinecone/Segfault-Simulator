#pragma once

#include "engine/core/rendering/lighting.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_uint2.hpp"
#include "glm/glm/ext/vector_uint3.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace sfs
{

/**
 * Clustered lighting: the view cut into clusters (screen tiles x depth
 * slices), each listing the point lights that can reach it, so a pixel only
 * looks at the few lights near it however many the scene has.
 *
 * Tiles are square, `tilePixels` across (16 to 64, about 16 tiles across the
 * viewport's shorter side), from the top left of the viewport.
 * Slices divide the depth along the view direction between the near and far
 * planes logarithmically (nearby slices thin, far ones deep, as perspective
 * wants). The renderer's shader finds a pixel's cluster the same way (see
 * meshLit.wgsl), so the two must agree.
 */
struct ClusterGrid
{
  glm::uvec3 size{1, 1, 1}; // tiles across, tiles down, depth slices
  float tilePixels = 64.0f; // forViewport() picks it for the viewport
  float nearPlane = 0.1f;
  float farPlane = 500.0f;

  /** A grid over a viewport `viewport` pixels big. */
  static ClusterGrid
  forViewport(glm::vec2 viewport, float nearPlane, float farPlane);

  uint32_t count() const { return size.x * size.y * size.z; }

  /** The slice a depth along the view direction falls in (clamped). */
  uint32_t sliceOf(float depth) const;

  /** The tile a pixel is in (clamped). */
  glm::uvec2 tileOf(glm::vec2 pixel) const;

  /** A cluster's index in ClusterLights::ranges. */
  uint32_t index(glm::uvec2 tile, uint32_t slice) const
  {
    return (slice * size.y + tile.y) * size.x + tile.x;
  }
};

/** The view a grid is laid over. */
struct ClusterView
{
  glm::mat4 viewProjection{1.0f};
  glm::vec2 viewport{1.0f};
  glm::vec3 cameraPosition{0.0f};
  glm::vec3 forward{0.0f, 0.0f, -1.0f}; // unit view direction
};

/** Each cluster's lights, as the GPU reads them. */
struct ClusterLights
{
  // Per cluster: where its lights start in `indices`, and how many.
  std::vector<glm::uvec2> ranges;
  // Indices into the frame's light list, cluster after cluster.
  std::vector<uint32_t> indices;
  // Clusters that had more lights than the cap (their extras were dropped).
  uint32_t overfull = 0;
};

/** Most lights one cluster lists; the earliest in the frame's list win. */
inline constexpr uint32_t kMaxLightsPerCluster = 256;

/**
 * List each light in every cluster its sphere might reach: the tiles its
 * bounds cover on screen, and the slices its depth range covers. It errs
 * towards listing a light where it can't quite reach (the shader checks the
 * distance); it never leaves one out of a cluster it can reach. Lights
 * entirely off screen, or nearer than the near plane or farther than the far
 * one, are in no cluster. `lights` is in priority order: a cluster that
 * would list more than `maxPerCluster` keeps the first ones.
 */
void assignLights(const ClusterGrid& grid,
                  const ClusterView& view,
                  std::span<const PointLight> lights,
                  ClusterLights& out,
                  uint32_t maxPerCluster = kMaxLightsPerCluster);

} // namespace sfs
