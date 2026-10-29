#include "engine/core/rendering/3d/aabb.h"

#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"

namespace sfs
{

void Aabb::expand(const glm::vec3& point)
{
  min = glm::min(min, point);
  max = glm::max(max, point);
}

void Aabb::expand(const Aabb& other)
{
  if (other.empty())
    return;

  min = glm::min(min, other.min);
  max = glm::max(max, other.max);
}

bool Aabb::intersectsSphere(const glm::vec3& center, float radius) const
{
  if (empty())
    return false;
  // The box's nearest point to the centre.
  const glm::vec3 nearest = glm::clamp(center, min, max);
  const glm::vec3 offset = nearest - center;
  return glm::dot(offset, offset) <= radius * radius;
}

Aabb Aabb::transformed(const glm::mat4& transform) const
{
  if (empty())
    return {};

  // Start from the translation, then add each matrix column's contribution
  // along the axis that grows the box on that side.
  const glm::vec3 translation{transform[3]};
  Aabb out;
  out.min = translation;
  out.max = translation;

  for (int column = 0; column < 3; ++column)
  {
    for (int row = 0; row < 3; ++row)
    {
      const float a = transform[column][row] * min[column];
      const float b = transform[column][row] * max[column];
      out.min[row] += glm::min(a, b);
      out.max[row] += glm::max(a, b);
    }
  }

  return out;
}

} // namespace sfs
