#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include <vector>

namespace sfs
{

// One particle's screen-space billboard: four corners, UVs, and colour. Unlit --
// particles carry their own colour rather than sampling scene lighting.
struct ParticleQuad
{
  glm::vec2 points[4] = {};
  glm::vec2 uvs[4] = {
      {0.0f, 0.0f},
      {1.0f, 0.0f},
      {1.0f, 1.0f},
      {0.0f, 1.0f},
  };
  glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};

  // Holds the world painter sort-key when the batch is built; the render system's
  // assignClipDepth() remaps it in place to clip-space depth (gl_Position.z) so
  // each particle occludes against terrain individually.
  float z = 0.0f;
};

// A batch of particles sharing one texture + blend mode (one draw call).
struct ParticleBatch
{
  std::vector<ParticleQuad> quads;
};

} // namespace sfs
