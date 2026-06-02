

#include "engine/systems/isometric/isometricLightingSystem.h"
#include "engine/components/lightEmitterComponent.h"
#include "engine/components/transformComponent.h"

#include "engine/utils/isometricLightingUtils.h"

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
        entity.getId(),
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

} // namespace sfs
