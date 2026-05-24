#pragma once

#include "engine/ecs/registry.h"
#include "engine/utils/isometricLightingUtils.h"
#include <vector>

namespace sfs
{

class IsometricLightingService
{
public:
  void setRegistry(Registry* registry);

  void setAmbientLighting(IsometricAmbientLighting ambient);
  const IsometricAmbientLighting* ambient() const;

  void updateCacheIfDirty();
  void invalidateCache();

  const std::vector<IsometricPointLightSnapshot>& pointLights() const;

  IsometricComputedLighting
  computeLighting(const IsometricLightingSample& sample) const;

private:
  Registry* m_registry = nullptr;
  IsometricAmbientLighting m_ambient;
  bool m_hasAmbient = false;

  std::vector<IsometricPointLightSnapshot> m_pointLights;
  bool m_lightsDirty = true;
};

} // namespace sfs
