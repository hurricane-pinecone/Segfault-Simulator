#pragma once

#include "engine/core/ecs/entity.h"
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

  // Emitting entity, so the render system can pair the light with the smoothed
  // ground elevation it tracks per actor (InvalidId if not from an entity).
  Entity::EntityId entityId = Entity::InvalidId;
};

struct IsometricAmbientLighting
{
  glm::vec3 direction{0.0f, 0.0f, 1.0f};
  glm::vec3 color{1.0f, 1.0f, 1.0f};
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
  float terrainShadowMaxLength = 1.25f;
  float spriteShadowMaxLength = 1.25f;

  float terrainShadowAlpha = 1.0f;
  float spriteShadowAlpha = 1.0f;
};

glm::vec2 gridDirectionToIsometricDirection(const glm::vec2& worldDir,
                                            float worldScale,
                                            int tileWidth,
                                            int tileHeight);

// Length a directional (sun) shadow reaches for a caster of the given height in
// elevation levels. Shared by terrain and sprite shadows so an edge and a
// sprite of the same height throw the same shadow. projectionFactor is the
// horizontal reach per unit height (horizontalAmount / sunHeight); the cap
// scales with height so tall casters keep their reach.
float projectedShadowLength(float heightLevels,
                            float projectionFactor,
                            float maxLength);

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
