#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float4.hpp"

namespace sfs
{

/**
 * Ground-space collision footprint on the tile grid: terrain cliff blocking,
 * solid objects (walls, lamps), and the terrain elevation / step-up the entity
 * stands on. A world object that should block movement carries this (plus the
 * `SolidObject` tag for solid-vs-solid).
 *
 * `offset`/`size` are authored in PIXELS relative to the sprite's feet (the
 * transform sits at the sprite's bottom edge), with pixel-x mapping to world-x
 * and pixel-y to world depth (y). They convert to world tiles via
 * `pixelsPerUnit` (the source tile width). Deliberately render-scale
 * independent: `worldScale` magnifies sprites AND tiles together at draw time,
 * so the footprint already grows on screen with the sprite without collision
 * changing when the view is zoomed.
 *
 * Solver: AABB-vs-AABB on the ground plane, gated by matching elevation. For
 * billboard / bullet hits use BoxCollider2D instead.
 *
 * @param glm::vec2 offset - footprint offset in pixels from the feet
 * @param glm::vec2 size - footprint size in pixels
 */
struct WorldCollider
{
  // Pixels per world tile, converting pixel offset/size to world units. Set
  // once at startup from the projection's tile width (defaults to 32px tiles).
  inline static float pixelsPerUnit = 32.0f;

  glm::vec2 offset; // pixels, from the feet
  glm::vec2 size;   // pixels
  glm::vec4 bounds; // world tiles, recomputed in updateBounds

  WorldCollider(glm::vec2 offset, glm::vec2 size)
      : offset(offset), size(size), bounds(0.0f)
  {
  }

  // Offset/size in world tiles (pixels divided by the tile pixel width).
  glm::vec2 worldOffset() const { return offset / pixelsPerUnit; }
  glm::vec2 worldSize() const { return size / pixelsPerUnit; }

  void updateBounds(const glm::vec2& transformPosition)
  {
    const glm::vec2 worldPosition = transformPosition + worldOffset();
    const glm::vec2 ws = worldSize();

    bounds = {worldPosition.x,
              worldPosition.y,
              worldPosition.x + ws.x,
              worldPosition.y + ws.y};
  }

  bool intersects(const WorldCollider& other) const
  {
    return bounds.x < other.bounds.z && bounds.z > other.bounds.x &&
           bounds.y < other.bounds.w && bounds.w > other.bounds.y;
  }

  float left() const { return bounds.x; }
  float top() const { return bounds.y; }
  float right() const { return bounds.z; }
  float bottom() const { return bounds.w; }
};

} // namespace sfs
