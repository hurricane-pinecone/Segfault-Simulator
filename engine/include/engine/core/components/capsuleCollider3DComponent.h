#pragma once

#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

/**
 * An upright capsule collider (a cylinder with rounded ends, along the
 * entity's Z axis): the usual shape for characters, since it slides over
 * small bumps. See BoxCollider3DComponent.
 *
 * @param float radius - of the rounded ends and the middle
 * @param float height - total height, ends included (at least 2 * radius)
 * @param glm::vec3 offset - centre relative to the entity's origin
 * @param bool trigger - detects bodies passing through instead of blocking them
 * @param int layer - what it is, for queries that only count some things
 *                    (CollisionWorld3D: bit n of a mask is layer n)
 */
struct CapsuleCollider3DComponent
{
  [[sfs::range(0.01, 20)]] float radius = 0.4f;
  [[sfs::range(0.02, 50)]] float height = 1.8f;
  glm::vec3 offset{0.0f};
  bool trigger = false;
  [[sfs::range(0, 31)]] int layer = 0;
};

} // namespace sfs
