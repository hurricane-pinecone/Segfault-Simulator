#pragma once

#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{
struct LightEmitterComponent
{
  glm::vec3 color{1.0f, 0.9f, 0.7f};

  float intensity = 2.0f;
  float radius = 250.0f;
  float height = 64.0f;

  LightEmitterComponent() = default;

  LightEmitterComponent(float radius, float intensity, float height = 64.0f)
      : intensity(intensity), radius(radius), height(height)
  {
  }
};
} // namespace sfs
