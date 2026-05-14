
#include "engine/systems/isometricLightingSystem.h"

#include "engine/components/lightEmitterComponent.h"
#include "engine/components/transformComponent.h"
#include "engine/ecs/registry.h" // Forward declarations

#include "glm/glm/geometric.hpp"

#include <algorithm>

namespace sfs
{

IsometricLightingSystem::IsometricLightingSystem(AssetStore& assetStore)
    : assetStore(assetStore)
{
  registerComponent<TransformComponent>();
  registerComponent<LightEmitterComponent>();
}

void IsometricLightingSystem::rebuildLights()
{
  lights.clear();

  for (const auto& entity : getEntities())
  {
    if (!entity.hasComponent<TransformComponent>() ||
        !entity.hasComponent<LightEmitterComponent>())
    {
      continue;
    }

    const auto& transform = entity.getComponent<TransformComponent>();
    const auto& light = entity.getComponent<LightEmitterComponent>();

    lights.push_back({transform.position,
                      light.height,
                      light.color,
                      light.intensity,
                      light.radius});
  }
}

void IsometricLightingSystem::setLightDirection(const glm::vec3& direction)
{
  if (glm::length(direction) < 0.001f)
    return;

  m_lightDirection = glm::normalize(direction);
}

void IsometricLightingSystem::setLighting(float ambient, float diffuseStrength)
{
  m_ambient = std::clamp(ambient, 0.0f, 1.0f);
  m_diffuseStrength = std::clamp(diffuseStrength, 0.0f, 2.0f);
}

IsometricComputedLighting IsometricLightingSystem::computeLighting(
    const IsometricLightingSample& sample) const
{
  glm::vec3 accumulatedLightDir{0.0f};
  float accumulatedIntensity = 0.0f;

  for (const auto& light : lights)
  {
    glm::vec2 toLightWorld2 = light.worldPosition - sample.worldPosition;
    float distance = glm::length(toLightWorld2);

    if (distance > light.radius)
      continue;

    float attenuation = 1.0f - distance / light.radius;
    attenuation = std::clamp(attenuation, 0.0f, 1.0f);

    // Softer falloff than squared.
    attenuation = std::pow(attenuation, 0.65f);

    glm::vec3 toLight{toLightWorld2.x,
                      toLightWorld2.y,
                      light.height - sample.elevationOffset};

    if (glm::length(toLight) > 0.001f)
    {
      accumulatedIntensity += attenuation * light.intensity;

      accumulatedLightDir +=
          glm::normalize(toLight) * attenuation * light.intensity;
    }
  }

  glm::vec3 lightDir =
      m_lightDirection * m_diffuseStrength + accumulatedLightDir;

  if (glm::length(lightDir) < 0.001f)
    lightDir = glm::vec3{0.0f, 0.0f, 1.0f};
  else
    lightDir = glm::normalize(lightDir);

  return {lightDir, accumulatedIntensity, m_ambient, m_diffuseStrength};
}

} // namespace sfs
