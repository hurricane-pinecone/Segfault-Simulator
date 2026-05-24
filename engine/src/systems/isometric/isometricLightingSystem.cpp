

#include "engine/systems/isometric/isometricLightingSystem.h"
#include "engine/components/lightEmitterComponent.h"
#include "engine/components/transformComponent.h"
#include "engine/ecs/registry.h" // IWYU pragma: keep

#include "engine/utils/isometricLightingUtils.h"
#include "glm/glm/geometric.hpp"

namespace sfs
{

void IsometricLightingService::setRegistry(Registry* registry)
{
  m_registry = registry;
  invalidateCache();
}

void IsometricLightingService::setAmbientLighting(
    IsometricAmbientLighting ambient)
{
  m_ambient = ambient;
  m_hasAmbient = true;
}

const IsometricAmbientLighting* IsometricLightingService::ambient() const
{
  return m_hasAmbient ? &m_ambient : nullptr;
}

void IsometricLightingService::updateCacheIfDirty()
{
  if (!m_lightsDirty)
    return;

  m_pointLights.clear();

  if (!m_registry)
  {
    m_lightsDirty = false;
    return;
  }

  for (const auto& entity :
       m_registry->view<TransformComponent, LightEmitterComponent>())
  {
    const auto& transform = entity.getComponent<TransformComponent>();
    const auto& light = entity.getComponent<LightEmitterComponent>();

    m_pointLights.push_back({
        transform.position,
        light.height,
        light.color,
        light.intensity,
        light.radius,
    });
  }

  m_lightsDirty = false;
}

void IsometricLightingService::invalidateCache() { m_lightsDirty = true; }

const std::vector<IsometricPointLightSnapshot>&
IsometricLightingService::pointLights() const
{
  return m_pointLights;
}

IsometricComputedLighting IsometricLightingService::computeLighting(
    const IsometricLightingSample& sample) const
{
  glm::vec3 accumulatedLightDir{0.0f};
  glm::vec3 accumulatedColor{0.0f};
  float accumulatedIntensity = 0.0f;

  for (const auto& light : m_pointLights)
  {
    const glm::vec2 toLightWorld2 = light.worldPosition - sample.worldPosition;

    const float distance = glm::length(toLightWorld2);

    if (distance > light.radius)
      continue;

    float attenuation = 1.0f - distance / light.radius;
    attenuation = std::clamp(attenuation, 0.0f, 1.0f);
    attenuation = std::pow(attenuation, 3.0f);

    const glm::vec3 toLight{
        toLightWorld2.x,
        toLightWorld2.y,
        light.height - sample.elevationOffset,
    };

    if (glm::length(toLight) <= 0.001f)
      continue;

    const float contribution = attenuation * light.intensity;

    accumulatedIntensity += contribution;
    accumulatedColor += light.color * contribution;
    accumulatedLightDir += glm::normalize(toLight) * contribution;
  }

  glm::vec3 finalColor{1.0f};
  if (accumulatedIntensity > 0.001f)
    finalColor = accumulatedColor / accumulatedIntensity;

  glm::vec3 ambientDirection{0.0f, 0.0f, 1.0f};
  float ambient = 1.0f;
  float diffuseStrength = 0.0f;

  if (m_hasAmbient)
  {
    ambientDirection = m_ambient.direction;
    ambient = m_ambient.ambient;
    diffuseStrength = m_ambient.diffuseStrength;
  }

  glm::vec3 lightDir = ambientDirection * diffuseStrength + accumulatedLightDir;

  if (glm::length(lightDir) < 0.001f)
    lightDir = glm::vec3{0.0f, 0.0f, 1.0f};
  else
    lightDir = glm::normalize(lightDir);

  return {
      lightDir,
      finalColor,
      accumulatedIntensity,
      ambient,
      diffuseStrength,
  };
}

} // namespace sfs
