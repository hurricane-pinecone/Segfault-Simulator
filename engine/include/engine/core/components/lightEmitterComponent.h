#pragma once

#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

/**
 * A point light emitted from the entity's transform position.
 *
 * Units (resolved in IsometricRenderSystem against the active projection):
 *   radius    - reach, in screen pixels. Converted to world tiles for the
 *               lighting math by dividing by the on-screen tile width
 *               (tileWidth * worldScale). Beyond it the light contributes
 *               nothing.
 *   intensity - brightness multiplier (0 = off, 1 = full); unitless.
 *   height    - emitter height above the tile it sits on, in screen pixels.
 *               Converted to elevation levels for terrain occlusion by dividing
 *               by the on-screen level height (elevationStep * worldScale), and
 *               also tilts the per-pixel diffuse term.
 *   color     - linear RGB tint of the light.
 *
 * radius and height are screen pixels, so world scale currently affects both
 * (the conversions include worldScale). Drop that factor in the render system
 * to make them world-scale independent.
 *
 * @param float radius - reach in screen pixels, default 640
 * @param float intensity - brightness multiplier (0..1)
 * @param float height - emitter height in screen pixels, default 64
 * @param glm::vec3 color - linear RGB tint, default (1, 0.9, 0.7)
 */
struct LightEmitterComponent
{
  float intensity = 1.0f;
  float radius = 640.0f;
  float height = 64.0f;
  glm::vec3 color{1.0f, 0.9f, 0.7f};

  LightEmitterComponent() = default;

  LightEmitterComponent(float radius,
                        float intensity,
                        float height = 64.0f,
                        glm::vec3 color = {1.0f, 0.9f, 0.7f})
      : intensity(intensity), radius(radius), height(height), color(color)
  {
  }
};
} // namespace sfs
