#include "engine/core/rendering/aabb.h"

#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"

#include <algorithm>

namespace sfs
{

float largestScale(const glm::mat4& transform)
{
  return std::max({glm::length(glm::vec3(transform[0])),
                   glm::length(glm::vec3(transform[1])),
                   glm::length(glm::vec3(transform[2]))});
}

glm::vec4
placedSphere(const glm::mat4& model, const glm::vec3& center, float radius)
{
  return glm::vec4(
      glm::vec3(model * glm::vec4(center, 1.0f)), radius * largestScale(model));
}

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
