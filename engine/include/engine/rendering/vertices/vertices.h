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

  // Painter sort-key at build time; assignClipDepth() remaps it in place to a
  // clip-space z (gl_Position.z) so a merged water mesh occludes per vertex.
  float z = 0.0f;
};

} // namespace sfs
