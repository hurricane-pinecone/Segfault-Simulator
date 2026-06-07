#pragma once

#include <glm/glm/ext/vector_float2.hpp>

namespace sfs
{

/**
 * An entity's 2D placement in world units: position, scale, and rotation.
 * Almost every entity has one -- the render path projects position to the
 * screen and the movement system writes it. The constructor seeds
 * previousPosition from position, so the first frame reports no movement.
 *
 * @param glm::vec2 position - world position, default (0, 0)
 * @param glm::vec2 scale - per-axis scale, default (1, 1)
 * @param double rotation - rotation, default 0
 */
struct TransformComponent
{
  glm::vec2 position;
  glm::vec2 previousPosition; // position last frame (motion / interpolation)
  glm::vec2 scale;
  double rotation;

  TransformComponent(glm::vec2 position = glm::vec2(0, 0),
                     glm::vec2 scale = glm::vec2(1, 1),
                     double rotation = 0.0)
      : position(position), previousPosition(position), scale(scale),
        rotation(rotation)
  {
  }
};

} // namespace sfs
