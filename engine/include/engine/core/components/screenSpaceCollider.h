#pragma once

#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

/**
 * Billboard hit box in the sprite's pixel space: `offset`/`size` are pixels
 * measured from the sprite's TOP-LEFT, so the box is authored by drawing it
 * straight over the sprite art. It resolves to a screen-space rectangle on
 * demand from the sprite size + projection (the isometric projection already
 * bakes elevation into the sprite's screen position, so this box carries no
 * z). Bullet / sprite hits test 2D overlap there.
 *
 * For ground movement and solid-object blocking use WorldCollider instead.
 */
struct ScreenSpaceCollider
{
  glm::vec2 offset; // pixels, from the sprite top-left
  glm::vec2 size;   // pixels

  ScreenSpaceCollider(glm::vec2 offset, glm::vec2 size)
      : offset(offset), size(size)
  {
  }
};

} // namespace sfs
