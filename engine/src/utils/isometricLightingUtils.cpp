#include "engine/utils/isometricLightingUtils.h"

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/geometric.hpp"
#include <algorithm>

namespace sfs
{

glm::vec2 gridDirectionToIsometricDirection(const glm::vec2& worldDir,
                                            float worldScale,
                                            int tileWidth,
                                            int tileHeight)
{
  glm::vec2 isoDir{
      (worldDir.x - worldDir.y) * (static_cast<float>(tileWidth) * worldScale) /
          2.0f,

      (worldDir.x + worldDir.y) *
          (static_cast<float>(tileHeight) * worldScale) / 2.0f,
  };

  if (glm::length(isoDir) < 0.001f)
    return {0.0f, 0.0f};

  return glm::normalize(isoDir);
}

std::vector<IsometricShadowResult>
computeIsometricShadows(const std::vector<IsometricLightSnapshot>& lights,
                        const glm::vec3& sunDirection,
                        float diffuseStrength,
                        const glm::vec2& casterWorldPosition,
                        int casterPixelHeight,
                        float worldScale,
                        int tileWidth,
                        int tileHeight)
{
  std::vector<IsometricShadowResult> shadows;

  for (const auto& light : lights)
  {
    glm::vec2 delta = casterWorldPosition - light.worldPosition;
    float distance = glm::length(delta);

    if (distance > light.radius || distance < 0.001f)
      continue;

    glm::vec2 shadowWorldDir = delta / distance;

    float attenuation = 1.0f - distance / light.radius;
    attenuation = std::pow(std::clamp(attenuation, 0.0f, 1.0f), 0.75f);

    float casterHeight = static_cast<float>(casterPixelHeight) * 0.75f;
    float visualLightHeight = std::max(light.height * 0.0625f, 1.0f);
    float shadowLength = casterHeight * distance / visualLightHeight;

    glm::vec2 isoDir = gridDirectionToIsometricDirection(
        shadowWorldDir, worldScale, tileWidth, tileHeight);

    float alpha = 0.42f * attenuation * std::clamp(light.intensity, 0.0f, 2.0f);

    shadows.push_back({isoDir * shadowLength, alpha});
  }

  float horizonFade = glm::smoothstep(0.0f, 0.15f, sunDirection.z);

  if (diffuseStrength > 0.001f && horizonFade > 0.001f)
  {
    glm::vec2 sunHorizontal{sunDirection.x, sunDirection.y};
    float horizontalLength = glm::length(sunHorizontal);

    if (horizontalLength > 0.001f)
    {
      glm::vec2 shadowWorldDir = -sunHorizontal / horizontalLength;

      glm::vec2 isoDir = gridDirectionToIsometricDirection(
          shadowWorldDir, worldScale, tileWidth, tileHeight);

      if (glm::length(isoDir) > 0.001f)
      {
        float noonFade = glm::smoothstep(0.0f, 0.25f, horizontalLength);

        float lowSunStretch = std::clamp(
            horizontalLength / std::max(sunDirection.z, 0.03f), 0.0f, 12.0f);

        float shadowLength = static_cast<float>(casterPixelHeight) *
                             lowSunStretch * 0.45f * noonFade;

        float shadowAlpha = 0.22f * noonFade * horizonFade;

        if (shadowLength > 0.5f && shadowAlpha > 0.01f)
          shadows.push_back({isoDir * shadowLength, shadowAlpha});
      }
    }
  }

  return shadows;
}

} // namespace sfs
