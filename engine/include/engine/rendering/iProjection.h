#pragma once

#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

/**
 * World-to-screen projection. Defines how a render system maps world-space
 * positions to screen pixels (and back). A render system that only needs the
 * transform can depend on this interface rather than a concrete projection;
 * IsometricProjection is the isometric heightfield implementation.
 */
class IProjection
{
public:
  virtual ~IProjection() = default;

  /**
   * Project a world position to screen pixels.
   *
   * @param world     world position
   * @param elevation elevation in the implementation's units
   * @return screen-pixel position
   */
  virtual glm::vec2 worldToScreen(const glm::vec2& world,
                                  float elevation) const = 0;

  /**
   * Inverse of worldToScreen: the world position under a screen pixel, assuming
   * it lies on the plane at the given elevation.
   *
   * @param screen    screen-pixel position
   * @param elevation elevation of the plane the result lies on
   * @return world position
   */
  virtual glm::vec2 screenToWorld(const glm::vec2& screen,
                                  float elevation) const = 0;
};

} // namespace sfs
