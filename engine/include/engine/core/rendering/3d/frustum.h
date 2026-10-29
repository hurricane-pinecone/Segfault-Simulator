#pragma once

#include "engine/core/rendering/3d/aabb.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float4.hpp"

#include <array>

namespace sfs
{

/**
 * The six clip planes of a view-projection (depth range [0, 1]). Planes point
 * inward: a point p is inside a plane when dot(plane.xyz, p) + plane.w >= 0.
 */
class Frustum
{
public:
  Frustum() = default;

  /** Extract the planes of a [0, 1]-depth view-projection matrix. */
  static Frustum fromViewProjection(const glm::mat4& viewProjection);

  /**
   * Conservative box test: false only when the box is entirely outside one
   * plane, so it may keep a few boxes near frustum corners but never drops a
   * visible one.
   */
  bool intersects(const Aabb& box) const;

  /**
   * Its planes (left, right, bottom, top, near, far): xyz a unit normal into
   * it, w its offset, so a point's inside where dot(xyz, p) + w >= 0 for all.
   */
  const std::array<glm::vec4, 6>& planes() const { return m_planes; }

private:
  std::array<glm::vec4, 6> m_planes{};
};

} // namespace sfs
