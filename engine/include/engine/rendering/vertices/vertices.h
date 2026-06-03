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

// A decal vertex in WORLD space. The decal vertex shader projects it to the
// screen and derives clip-space z from `sortKey` (using the frame's depth range
// uniforms), so settled decals live unchanged in GPU buffers across camera moves.
struct DecalVertex
{
  glm::vec2 worldPos{0.0f, 0.0f}; // world tile coords
  float elevation = 0.0f;         // elevation level
  glm::vec2 uv{0.0f, 0.0f};
  glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
  float sortKey = 0.0f; // world painter key (matches terrain tile keys)
};

} // namespace sfs
