#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include <cstdint>

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

// A vertex of opt-in block geometry (real terrain faces: tops + sides). Built
// per frame by the BlockGeometry module. `position` is screen pixels (the
// renderer converts to NDC); `worldPos` + `ground` (elevation level) drive the
// per-pixel point lighting (a side face's ground varies down the face, so it
// lights from its base up); `normal` is the real world-space face normal; `z`
// is the world painter sort-key that assignClipDepth remaps to clip-space depth
// per vertex.
struct GeometryVertex
{
  glm::vec2 position{0.0f, 0.0f};
  glm::vec2 worldPos{0.0f, 0.0f};
  float ground = 0.0f;
  glm::vec2 uv{0.0f, 0.0f};
  glm::vec3 normal{0.0f, 0.0f, 1.0f};
  float z = 0.0f;
};

// A decal vertex in WORLD space. The decal vertex shader projects it to the
// screen and derives clip-space z from `sortKey` (using the frame's depth range
// uniforms), so settled decals live unchanged in GPU buffers across camera
// moves.
struct DecalVertex
{
  glm::vec2 worldPos{0.0f, 0.0f}; // world tile coords
  float elevation = 0.0f;         // elevation level
  glm::vec2 uv{0.0f, 0.0f};
  std::uint32_t color = 0xFFFFFFFF; // packed RGBA8 (uploaded as a normalised
                                    // ubyte4 attribute -> vec4 in the shader)
  float sortKey = 0.0f;             // world painter key (matches terrain tiles)
};

} // namespace sfs
