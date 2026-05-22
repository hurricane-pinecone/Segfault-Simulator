#pragma once

#include "SDL_pixels.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include <SDL_rect.h>
#include <vector>

namespace sfs
{

struct Quad
{
  SDL_Color tint{255, 255, 255, 255};
  glm::vec2 points[4] = {};
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

static constexpr int MaxShaderLights = 16;

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

  int lightCount = 0;
  glm::vec2 lightPositions[MaxShaderLights] = {};
  glm::vec3 lightColors[MaxShaderLights] = {};
  float lightIntensities[MaxShaderLights] = {};
  float lightRadii[MaxShaderLights] = {};
  float lightHeights[MaxShaderLights] = {};
};

struct LitQuadBatch
{
  std::vector<LitQuad> quads;
};

} // namespace sfs
