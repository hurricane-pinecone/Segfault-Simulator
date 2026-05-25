#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float4.hpp"

namespace sfs
{

struct SurfaceVertex
{
  glm::vec2 position;
  glm::vec2 worldPosition;
  glm::vec4 color;
  glm::vec2 uv;
  glm::vec4 params;
};

} // namespace sfs
