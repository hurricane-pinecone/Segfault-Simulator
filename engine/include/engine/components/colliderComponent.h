#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float4.hpp"

namespace sfs
{

/**
 * Bounding rect of hit mask.
 * Add tag `sfs::SolidObject` to impede movement on collision.
 *
 * @param glm::vec2 offset
 * @param glm::vec2 size
 * @param glm::vec2 position
 */
struct ColliderComponent
{
  glm::vec2 offset;
  glm::vec2 size;
  glm::vec4 bounds;

  ColliderComponent(glm::vec2 offset, glm::vec2 size)
      : offset(offset), size(size), bounds(0.0f)
  {
  }

  void updateBounds(const glm::vec2& transformPosition)
  {
    glm::vec2 worldPosition = transformPosition + offset;

    bounds = {worldPosition.x,
              worldPosition.y,
              worldPosition.x + size.x,
              worldPosition.y + size.y};
  }

  bool intersects(const ColliderComponent& other) const
  {
    return bounds.x < other.bounds.z && bounds.z > other.bounds.x &&
           bounds.y < other.bounds.w && bounds.w > other.bounds.y;
  }

  float left() const { return bounds.x; }
  float top() const { return bounds.y; }
  float right() const { return bounds.z; }
  float bottom() const { return bounds.w; }
};

struct SolidObject
{
};

} // namespace sfs
