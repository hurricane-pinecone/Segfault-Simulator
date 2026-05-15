#pragma once

#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{
struct LightEmitterComponent
{

  float intensity = 1.0f;
  float radius = 10.0f;
  float height;
  glm::vec3 color;

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
