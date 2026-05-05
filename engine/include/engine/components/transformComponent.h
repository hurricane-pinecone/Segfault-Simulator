#pragma once

#include <glm/glm/ext/vector_float2.hpp>

namespace sfs
{

struct TransformComponent
{
  glm::vec2 position;
  glm::vec2 scale;
  double rotation;

  TransformComponent(glm::vec2 position = glm::vec2(0, 0),
                     glm::vec2 scale = glm::vec2(1, 1),
                     double rotation = 0.0)
      : position(position), scale(scale), rotation(rotation)
  {
  }
};

} // namespace sfs
