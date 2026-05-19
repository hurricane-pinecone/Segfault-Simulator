

#include "engine/systems/isometric/isometricLightingSystem.h"
#include "engine/components/lightEmitterComponent.h"
#include "engine/components/transformComponent.h"
#include "engine/ecs/registry.h" // IWYU pragma: keep

#include "engine/utils/isometricLightingUtils.h"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>

namespace sfs
{

IsometricLightingSystem::IsometricLightingSystem()
{
  registerComponent<TransformComponent>();
  registerComponent<LightEmitterComponent>();
}

void IsometricLightingSystem::rebuildLightSnapshots()
{
  m_cache.lights.clear();

  for (const auto& entity : getEntities())
  {
    if (!entity.hasComponent<TransformComponent>() ||
        !entity.hasComponent<LightEmitterComponent>())
    {
      continue;
    }

    const auto& transform = entity.getComponent<TransformComponent>();
    const auto& light = entity.getComponent<LightEmitterComponent>();

    m_cache.lights.push_back({
        transform.position,
        light.height,
        light.color,
        light.intensity,
        light.radius,
    });
  }
}

IsometricComputedLighting IsometricLightingSystem::computeLighting(
    const IsometricLightingSample& sample) const
{
  glm::vec3 accumulatedLightDir{0.0f, 0.0f, 0.0f};
  glm::vec3 accumulatedColor{0.0f, 0.0f, 0.0f};
  float accumulatedIntensity = 0.0f;

  for (const auto& light : m_cache.lights)
  {
    glm::vec2 toLightWorld2 = light.worldPosition - sample.worldPosition;

    float distance = glm::length(toLightWorld2);

    if (distance > light.radius)
      continue;

    float attenuation = 1.0f - distance / light.radius;
    attenuation = std::clamp(attenuation, 0.0f, 1.0f);
    attenuation = std::pow(attenuation, 3.0f);

    glm::vec3 toLight{
        toLightWorld2.x,
        toLightWorld2.y,
        light.height - sample.elevationOffset,
    };

    if (glm::length(toLight) > 0.001f)
    {
      const float contribution = attenuation * light.intensity;

      accumulatedIntensity += contribution;
      accumulatedColor += light.color * contribution;
      accumulatedLightDir += glm::normalize(toLight) * contribution;
    }
  }

  glm::vec3 finalColor{1.0f, 1.0f, 1.0f};

  if (accumulatedIntensity > 0.001f)
  {
    finalColor = accumulatedColor / accumulatedIntensity;
  }

  glm::vec3 lightDir =
      m_ambientLighting.direction * m_ambientLighting.diffuseStrength +
      accumulatedLightDir;

  if (glm::length(lightDir) < 0.001f)
    lightDir = glm::vec3{0.0f, 0.0f, 1.0f};
  else
    lightDir = glm::normalize(lightDir);

  return {
      lightDir,
      finalColor,
      accumulatedIntensity,
      m_ambientLighting.ambient,
      m_ambientLighting.diffuseStrength,
  };
}

void IsometricLightingSystem::submitLighting(
    const IsometricRenderContext& context,
    IsometricRenderQueue& queue)
{
  if (!m_cache.lightsDirty)
    return;

  rebuildLightSnapshots();
  m_cache.lightsDirty = false;
}

void IsometricLightingSystem::markLightsDirty() { m_cache.lightsDirty = true; }

void IsometricLightingSystem::setAmbientLighting(
    IsometricAmbientLighting ambient)
{
  m_ambientLighting = ambient;
}

void IsometricLightingSystem::setAmbient(float ambient)
{
  m_ambientLighting.ambient = ambient;
}

void IsometricLightingSystem::setAmbientDirection(glm::vec3 direction)
{
  if (glm::length(direction) < 0.001f)
    return;

  m_ambientLighting.direction = glm::normalize(direction);
}

void IsometricLightingSystem::setAmbientColor(glm::vec3 color)
{
  m_ambientLighting.color = color;
}

void IsometricLightingSystem::setAmbientDiffuseStrength(float strength)
{
  m_ambientLighting.diffuseStrength = strength;
}

} // namespace sfs
