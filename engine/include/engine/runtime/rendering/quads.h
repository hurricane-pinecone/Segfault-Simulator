#pragma once

#include "engine/core/particles/particleBatch.h" // ParticleQuad / ParticleBatch
#include "SDL_pixels.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include <SDL_rect.h>
#include <vector>

namespace sfs
{

struct Quad
{
  SDL_Color tint{255, 255, 255, 255};
  glm::vec2 points[4] = {};

  // Clip-space depth (gl_Position.z), set from the command's painter sort-key
  // so the depth buffer orders quads correctly.
  float z = 0.0f;
};

struct QuadBatch
{
  std::vector<Quad> quads;
};

struct TexturedQuad
{
  unsigned int texture = 0;

  SDL_Rect srcRect{0, 0, 0, 0};
  SDL_Rect destRect{0, 0, 0, 0};

  int textureWidth = 0;
  int textureHeight = 0;

  SDL_Color tint{255, 255, 255, 255};

  // Rotation in radians about the quad's centre (0 = axis-aligned). Lets a
  // sprite face an arbitrary direction (e.g. a projectile aligned to its
  // velocity).
  float rotation = 0.0f;

  // Clip-space depth (gl_Position.z), set from the command's painter sort-key
  // so the depth buffer orders quads correctly. z is the quad's bottom edge;
  // zTop is its top edge. A billboard standing upright is a vertical surface,
  // so its depth must increase with screen height the same way block-face
  // geometry does -- otherwise its single flat depth lets a wall it stands in
  // front of poke through it. depthSpan is the painter-key rise from bottom to
  // top (0 = flat: zTop resolves equal to z), filled by the render system from
  // the sprite's height in elevation levels; assignClipDepth maps it into zTop.
  float z = 0.0f;
  float zTop = 0.0f;
  float depthSpan = 0.0f;
};

struct FreeformQuad : Quad
{
  unsigned int texture = 0;

  SDL_Rect srcRect{0, 0, 0, 0};

  int textureWidth = 0;
  int textureHeight = 0;

  glm::vec2 uvs[4] = {
      {0.0f, 0.0f},
      {1.0f, 0.0f},
      {1.0f, 1.0f},
      {0.0f, 1.0f},
  };
};

static constexpr int MaxShaderLights = 128;

// The frame's point lights, shared by every lit/surface draw and bound once on
// the renderer.
struct PointLightSet
{
  int count = 0;
  glm::vec2 positions[MaxShaderLights] = {};
  glm::vec3 colors[MaxShaderLights] = {};
  float intensities[MaxShaderLights] = {};
  float radii[MaxShaderLights] = {};
  float heights[MaxShaderLights] = {};

  // The emitter's ground elevation in levels (terrain height under the light).
  // Supplied by the CPU rather than sampled from the heightmap in the shader so
  // a moving emitter can ease between tile elevations instead of snapping a
  // whole level as it crosses a tile border.
  float groundLevels[MaxShaderLights] = {};
};

struct LitQuad : TexturedQuad
{
  bool hasNormalMap = false;
  unsigned int normalTexture = 0;

  glm::vec3 lightDirection{0.0f, 0.0f, 1.0f};
  float lightIntensity = 1.0f;
  float ambient = 0.18f;
  float diffuseStrength = 0.85f;

  glm::vec3 lightColor{1.0f, 1.0f, 1.0f};

  // Shader-space world samples, not screen draw points.
  glm::vec2 worldPoints[4] = {};

  // EG, water, grass etc.
  int surfaceEffect = 0;
};

struct LitQuadBatch
{
  std::vector<LitQuad> quads;
};

} // namespace sfs
