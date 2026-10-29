#include "engine/core/rendering/3d/lightClusters.h"

#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>

namespace sfs
{

namespace
{
constexpr uint32_t kSlices = 24;
}

ClusterGrid
ClusterGrid::forViewport(glm::vec2 viewport, float nearPlane, float farPlane)
{
  // About 16 tiles across the shorter side, 16 to 64 pixels each: small
  // views (a preview, a small window) still split their lights up.
  ClusterGrid grid;
  grid.tilePixels = std::clamp(
      std::floor(std::min(viewport.x, viewport.y) / 16.0f), 16.0f, 64.0f);
  grid.size.x = std::max(
      1u, static_cast<uint32_t>(std::ceil(viewport.x / grid.tilePixels)));
  grid.size.y = std::max(
      1u, static_cast<uint32_t>(std::ceil(viewport.y / grid.tilePixels)));
  grid.size.z = kSlices;
  grid.nearPlane = std::max(nearPlane, 1e-3f);
  grid.farPlane = std::max(farPlane, grid.nearPlane * 1.01f);
  return grid;
}

uint32_t ClusterGrid::sliceOf(float depth) const
{
  // log(depth / near) / log(far / near) of the way through the slices.
  const float scale =
      static_cast<float>(size.z) / std::log(farPlane / nearPlane);
  const float slice =
      std::floor(std::log(std::max(depth, nearPlane) / nearPlane) * scale);
  return static_cast<uint32_t>(
      std::clamp(slice, 0.0f, static_cast<float>(size.z - 1)));
}

glm::uvec2 ClusterGrid::tileOf(glm::vec2 pixel) const
{
  const glm::vec2 tile = glm::floor(pixel / tilePixels);
  return {static_cast<uint32_t>(
              std::clamp(tile.x, 0.0f, static_cast<float>(size.x - 1))),
          static_cast<uint32_t>(
              std::clamp(tile.y, 0.0f, static_cast<float>(size.y - 1)))};
}

void assignLights(const ClusterGrid& grid,
                  const ClusterView& view,
                  std::span<const PointLight3D> lights,
                  ClusterLights& out,
                  uint32_t maxPerCluster)
{
  // Each light's cluster box (tiles and slices), or none.
  struct Box
  {
    glm::uvec2 tileMin, tileMax;
    uint32_t sliceMin, sliceMax;
  };
  std::vector<Box> boxes(lights.size());
  std::vector<bool> placed(lights.size(), false);
  for (std::size_t i = 0; i < lights.size(); ++i)
  {
    const PointLight3D& light = lights[i];
    const float depth =
        glm::dot(light.position - view.cameraPosition, view.forward);
    if (depth + light.radius < grid.nearPlane ||
        depth - light.radius > grid.farPlane)
      continue;

    // On screen: the bounds of its box's corners; all of it if any corner is
    // behind the camera (its projection wraps).
    glm::vec2 low(0.0f), high(view.viewport);
    bool behind = false;
    glm::vec2 cornerLow(1e30f), cornerHigh(-1e30f);
    for (int corner = 0; corner < 8 && !behind; ++corner)
    {
      const glm::vec3 offset((corner & 1) ? light.radius : -light.radius,
                             (corner & 2) ? light.radius : -light.radius,
                             (corner & 4) ? light.radius : -light.radius);
      const glm::vec4 clip =
          view.viewProjection * glm::vec4(light.position + offset, 1.0f);
      if (clip.w <= 1e-4f)
      {
        behind = true;
        break;
      }
      const glm::vec2 ndc = glm::vec2(clip) / clip.w;
      const glm::vec2 pixel((ndc.x * 0.5f + 0.5f) * view.viewport.x,
                            (0.5f - ndc.y * 0.5f) * view.viewport.y);
      cornerLow = glm::min(cornerLow, pixel);
      cornerHigh = glm::max(cornerHigh, pixel);
    }
    if (!behind)
    {
      if (cornerHigh.x < 0.0f || cornerHigh.y < 0.0f ||
          cornerLow.x > view.viewport.x || cornerLow.y > view.viewport.y)
        continue; // off screen
      low = cornerLow;
      high = cornerHigh;
    }
    boxes[i] = {grid.tileOf(low),
                grid.tileOf(high),
                grid.sliceOf(depth - light.radius),
                grid.sliceOf(depth + light.radius)};
    placed[i] = true;
  }

  // Count each cluster's lights, then lay the lists out one after another.
  out.ranges.assign(grid.count(), glm::uvec2(0));
  out.overfull = 0;
  const auto forEachCluster = [&](const Box& box, auto&& visit)
  {
    for (uint32_t slice = box.sliceMin; slice <= box.sliceMax; ++slice)
      for (uint32_t y = box.tileMin.y; y <= box.tileMax.y; ++y)
        for (uint32_t x = box.tileMin.x; x <= box.tileMax.x; ++x)
          visit(grid.index({x, y}, slice));
  };
  for (std::size_t i = 0; i < lights.size(); ++i)
    if (placed[i])
      forEachCluster(
          boxes[i], [&](uint32_t cluster) { ++out.ranges[cluster].y; });
  uint32_t offset = 0;
  for (glm::uvec2& range : out.ranges)
  {
    if (range.y > maxPerCluster)
    {
      range.y = maxPerCluster;
      ++out.overfull;
    }
    range.x = offset;
    offset += range.y;
    range.y = 0; // refilled below
  }
  out.indices.assign(offset, 0);
  std::vector<uint32_t> room(grid.count());
  for (std::size_t c = 0; c + 1 < out.ranges.size(); ++c)
    room[c] = out.ranges[c + 1].x - out.ranges[c].x;
  if (!out.ranges.empty())
    room.back() = offset - out.ranges.back().x;
  for (std::size_t i = 0; i < lights.size(); ++i)
    if (placed[i])
      forEachCluster(boxes[i],
                     [&](uint32_t cluster)
                     {
                       glm::uvec2& range = out.ranges[cluster];
                       if (range.y < room[cluster])
                         out.indices[range.x + range.y++] =
                             static_cast<uint32_t>(i);
                     });
}

} // namespace sfs
