#pragma once

#include "glm/ext/vector_float2.hpp"

struct RigidBodyComponent
{
  glm::vec2 velocity;

  RigidBodyComponent(glm::vec2 velocity = glm::vec2(0.0, 0.0))
  {
    this->velocity = velocity;
  }
};
