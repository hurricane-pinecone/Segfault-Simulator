#pragma once

#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

/**
 * A box-shaped collider, turned and scaled with the entity's transform. Gives
 * the entity a physics shape: static on its own, moving with a
 * RigidBody3DComponent. Colliders of different kinds on one entity combine.
 *
 * @param glm::vec3 size - full size along the entity's axes, default a unit
 * cube
 * @param glm::vec3 offset - centre relative to the entity's origin
 * @param bool trigger - detects bodies passing through (onTriggerEnter/Exit)
 *                       instead of blocking them
 * @param int layer - what it is, for queries that only count some things
 *                    (CollisionWorld3D: bit n of a mask is layer n)
 */
struct BoxCollider3DComponent
{
  glm::vec3 size{1.0f};
  glm::vec3 offset{0.0f};
  bool trigger = false;
  [[sfs::range(0, 31)]] int layer = 0;
};

} // namespace sfs
