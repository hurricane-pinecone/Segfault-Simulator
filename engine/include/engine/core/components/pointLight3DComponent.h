#pragma once

#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

/**
 * A point light carried by an entity with a Transform3DComponent. All units are
 * world units (unlike the isometric LightEmitterComponent's screen pixels).
 *
 * @param glm::vec3 color - linear RGB tint, default warm lamp
 * @param float intensity - brightness multiplier (0 = off), default 1
 * @param float radius - reach in world units, default 8
 * @param glm::vec3 offset - emitter position relative to the transform, e.g.
 *                           a lantern's flame above its base, default origin
 */
struct PointLight3DComponent
{
  glm::vec3 color{1.0f, 0.85f, 0.6f};
  float intensity = 1.0f;
  float radius = 8.0f;
  glm::vec3 offset{0.0f};

  PointLight3DComponent() = default;

  PointLight3DComponent(glm::vec3 color,
                        float intensity,
                        float radius,
                        glm::vec3 offset = glm::vec3(0.0f))
      : color(color), intensity(intensity), radius(radius), offset(offset)
  {
  }
};

} // namespace sfs
