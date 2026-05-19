#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <vector>

namespace sfs
{

struct IsometricPointLightSnapshot
{
  glm::vec2 worldPosition{0.0f, 0.0f};
  float height = 0.0f;
  glm::vec3 color{1.0f, 1.0f, 1.0f};
  float intensity = 1.0f;
  float radius = 1.0f;
};

struct IsometricAmbientLighting
{
  glm::vec3 direction{0.0f, 0.0f, 1.0f};
  glm::vec3 color{1.0f, 1.0f, 1.0f};
  float ambient = 0.18f;
  float diffuseStrength = 0.85f;
};

struct IsometricLightingSample
{
  glm::vec2 worldPosition;
  float elevationOffset = 0.0f;
};

struct IsometricComputedLighting
{
  glm::vec3 direction{0.0f, 0.0f, 1.0f};
  glm::vec3 color{1.0f, 1.0f, 1.0f};
  float intensity = 1.0f;
  float ambient = 0.18f;
  float diffuseStrength = 0.85f;
};

struct IsometricShadowResult
{
  glm::vec2 offset{0.0f, 0.0f};
  float alpha = 0.0f;
};

struct IsometricShadowSettings
{
  float terrainShadowMaxLength = 3.0f;
  float spriteShadowMaxLength = 1.25f;

  float terrainShadowAlpha = 1.0f;
  float spriteShadowAlpha = 1.0f;
};

glm::vec2 gridDirectionToIsometricDirection(const glm::vec2& worldDir,
                                            float worldScale,
                                            int tileWidth,
                                            int tileHeight);

std::vector<IsometricShadowResult>
computeIsometricShadows(const std::vector<IsometricPointLightSnapshot>& lights,
                        const glm::vec3& sunDirection,
                        float diffuseStrength,
                        const glm::vec2& casterWorldPosition,
                        int casterPixelHeight,
                        float worldScale,
                        int tileWidth,
                        int tileHeight);

} // namespace sfs
