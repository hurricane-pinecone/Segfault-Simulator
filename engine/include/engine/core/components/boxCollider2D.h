#pragma once

#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

/**
 * Axis-aligned 2D box collider: `half`-extents centred on the entity's
 * TransformComponent.position, optionally shifted by `offset`. Units are the
 * entity's local units, scaled by the active projection like the sprite is --
 * world pixels on the flat path (1:1 at zoom 1), sprite pixels on the isometric
 * path (the projection bakes elevation into the sprite's screen position, so
 * the box carries no z). Used for 2D overlap: platform/solid blocking, bullet
 * and sprite hits.
 *
 * For isometric GROUND movement and solid-object blocking (which need
 * elevation) use WorldCollider instead.
 *
 * @param glm::vec2 half - half-extents
 * @param glm::vec2 offset - centre offset from the entity, default (0, 0)
 */
struct BoxCollider2D
{
  glm::vec2 half{8.0f, 8.0f};
  glm::vec2 offset{0.0f, 0.0f};

  BoxCollider2D() = default;
  explicit BoxCollider2D(glm::vec2 half, glm::vec2 offset = {0.0f, 0.0f})
      : half(half), offset(offset)
  {
  }
};

} // namespace sfs
