#pragma once

#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

/**
 * Axis-aligned 2D box collider: `half`-extents centred on the entity's
 * TransformComponent.position, optionally shifted by `offset`, in the same
 * units as the position (a flat game's pixels). The box has no height. Used for
 * 2D overlap: platform/solid blocking, bullet and sprite hits.
 *
 * For isometric ground movement and solid-object blocking (which need height)
 * use WorldCollider instead.
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
