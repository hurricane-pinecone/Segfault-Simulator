#pragma once

#include <glm/glm/ext/vector_float2.hpp>
namespace sfs
{

struct CameraComponent
{
  int target;
  glm::vec2 offset{0.0f, 0.0f};
  float smoothing = 8.0f;
};

} // namespace sfs
