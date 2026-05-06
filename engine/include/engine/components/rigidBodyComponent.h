#pragma once

#include <glm/glm/ext/vector_float2.hpp>

namespace sfs
{

/**
 * @param glm::vec2 velocity
 */
struct RigidBodyComponent
{
  glm::vec2 velocity;

  RigidBodyComponent(glm::vec2 velocity = glm::vec2(0.0, 0.0))
      : velocity(velocity)
  {
  }
};

} // namespace sfs
