#pragma once

#include "engine/core/ecs/entity.h"

#include <glm/glm/ext/vector_float3.hpp>

namespace sfs
{

/** Where a raycast hit a physics body. */
struct RaycastHit3D
{
  Entity entity;
  glm::vec3 point{0.0f};
  glm::vec3 normal{0.0f, 0.0f, 1.0f};
  float distance = 0.0f;
};

} // namespace sfs
