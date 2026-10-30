#pragma once

#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

/**
 * Axis-aligned 2D box collider: `half`-extents centred on the entity's
 * position on the game's plane (TransformComponent::position2D(), in the
 * world), shifted by `offset`, in the same units as the position (a flat
 * game's pixels, an isometric game's tiles). The transform's scale and turn
 * don't change it.
 *
 * PhysicsSystem2D stops RigidBody2DComponent bodies against the ones without
 * a body (statics); a trigger blocks nothing, but is still told what passes
 * through it. Things only meet at the same height (the transform's z,
 * rounded), so an isometric game's levels don't collide.
 *
 * @param glm::vec2 half - half-extents
 * @param glm::vec2 offset - centre offset from the entity, default (0, 0)
 * @param bool trigger - overlaps are reported, nothing is blocked
 */
struct BoxCollider2D
{
  glm::vec2 half{8.0f, 8.0f};
  glm::vec2 offset{0.0f, 0.0f};
  bool trigger = false;

  BoxCollider2D() = default;
  explicit BoxCollider2D(glm::vec2 half,
                         glm::vec2 offset = {0.0f, 0.0f},
                         bool trigger = false)
      : half(half), offset(offset), trigger(trigger)
  {
  }
};

} // namespace sfs
