#pragma once

#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"

#include <limits>

namespace sfs
{

/**
 * Axis-aligned bounding box in 3D. Default-constructed it is empty (min > max),
 * so the first expand() snaps it to that point.
 */
struct Aabb
{
  glm::vec3 min{std::numeric_limits<float>::max()};
  glm::vec3 max{std::numeric_limits<float>::lowest()};

  bool empty() const { return min.x > max.x || min.y > max.y || min.z > max.z; }

  glm::vec3 center() const { return (min + max) * 0.5f; }

  /** Half-size on each axis. */
  glm::vec3 extents() const { return (max - min) * 0.5f; }

  void expand(const glm::vec3& point);
  void expand(const Aabb& other);

  /**
   * The box that encloses this box after an affine transform. Exact for
   * rotations and scales (Arvo's method), so a rotated mesh is never culled
   * while any part of it is visible.
   */
  Aabb transformed(const glm::mat4& transform) const;

  /** Whether any of the box is within `radius` of `center`. */
  bool intersectsSphere(const glm::vec3& center, float radius) const;
};

/**
 * The largest of an affine transform's scales along its axes: how much a
 * sphere's radius grows under it.
 * @param transform the transform, e.g. a model matrix
 * @return the length of its longest axis
 */
float largestScale(const glm::mat4& transform);

/**
 * A sphere in a model's own space, placed by its model matrix: its centre
 * moved, its radius grown by largestScale().
 * @param model the model matrix
 * @param center the sphere's centre in the model's space
 * @param radius its radius in the model's space
 * @return the placed sphere, xyz its centre and w its radius
 */
glm::vec4
placedSphere(const glm::mat4& model, const glm::vec3& center, float radius);

} // namespace sfs
