#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <vector>

namespace sfs
{

struct IsometricLightSnapshot
{
  glm::vec2 worldPosition{0.0f, 0.0f};
  float height = 0.0f;
  glm::vec3 color{255, 255, 255};
  float intensity = 1.0f;
  float radius = 1.0f;
};

struct IsometricShadowResult
{
  glm::vec2 offset{0.0f, 0.0f};
  float alpha = 0.0f;
};

glm::vec2 gridDirectionToIsometricDirection(const glm::vec2& worldDir,
                                            float worldScale,
                                            int tileWidth,
                                            int tileHeight);

std::vector<IsometricShadowResult>
computeIsometricShadows(const std::vector<IsometricLightSnapshot>& lights,
                        const glm::vec3& sunDirection,
                        float diffuseStrength,
                        const glm::vec2& casterWorldPosition,
                        int casterPixelHeight,
                        float worldScale,
                        int tileWidth,
                        int tileHeight);

} // namespace sfs
