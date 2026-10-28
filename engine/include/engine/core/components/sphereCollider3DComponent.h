#pragma once

#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

/**
 * A sphere-shaped collider (see BoxCollider3DComponent). A scaled entity
 * scales the radius by its largest scale axis, so it stays a sphere.
 *
 * @param float radius - default fits a unit cube
 * @param glm::vec3 offset - centre relative to the entity's origin
 * @param bool trigger - detects bodies passing through instead of blocking them
 */
struct SphereCollider3DComponent
{
  [[sfs::range(0.01, 50)]] float radius = 0.5f;
  glm::vec3 offset{0.0f};
  bool trigger = false;
};

} // namespace sfs
