#include "engine/core/rendering/3d/frustum.h"

#include "glm/glm/geometric.hpp"
#include "glm/glm/matrix.hpp"

namespace sfs
{

Frustum Frustum::fromViewProjection(const glm::mat4& viewProjection)
{
  // Gribb-Hartmann, on the matrix rows. With depth in [0, 1] the near plane is
  // row 2 alone (z >= 0) rather than row 3 + row 2.
  const glm::mat4 m = glm::transpose(viewProjection);

  Frustum frustum;
  frustum.m_planes = {
      m[3] + m[0], // left
      m[3] - m[0], // right
      m[3] + m[1], // bottom
      m[3] - m[1], // top
      m[2],        // near
      m[3] - m[2], // far
  };

  for (glm::vec4& plane : frustum.m_planes)
    plane /= glm::length(glm::vec3(plane));

  return frustum;
}

bool Frustum::intersects(const Aabb& box) const
{
  if (box.empty())
    return false;

  for (const glm::vec4& plane : m_planes)
  {
    // The box corner furthest along the plane normal; if even that is behind
    // the plane, the whole box is.
    const glm::vec3 corner{plane.x >= 0.0f ? box.max.x : box.min.x,
                           plane.y >= 0.0f ? box.max.y : box.min.y,
                           plane.z >= 0.0f ? box.max.z : box.min.z};

    if (glm::dot(glm::vec3(plane), corner) + plane.w < 0.0f)
      return false;
  }

  return true;
}

} // namespace sfs
