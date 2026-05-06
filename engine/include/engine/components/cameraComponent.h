#pragma once

#include <glm/glm/ext/vector_float2.hpp>
namespace sfs
{

/**
 * @param int target - The entity id if attaching camera to an entity
 * @param glm::vec2 offset
 * @param float smoothing
 */
struct CameraComponent
{
  int target;
  glm::vec2 offset{0.0f, 0.0f};
  float smoothing = 8.0f;
};

} // namespace sfs
