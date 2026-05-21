#pragma once

#include "engine/ecs/system.h"
#include "engine/renderers/commands/commands.h"
#include "engine/renderers/isometricRenderContext.h"
#include "engine/renderers/renderQueue.h"
#include "engine/utils/isometricLightingUtils.h"
#include "glm/glm/ext/vector_float3.hpp"
#include <vector>

namespace sfs
{

class IsometricLightingSystem : public System
{
public:
  IsometricLightingSystem();

  void submitLighting(const IsometricRenderContext& context,
                      RenderQueue<AnyRenderCommand>& queue);

  void markLightsDirty();

  IsometricComputedLighting
  computeLighting(const IsometricLightingSample& sample) const;

  const std::vector<IsometricPointLightSnapshot>& getPointLights() const
  {
    return m_cache.lights;
  }

  const IsometricAmbientLighting& ambient() const { return m_ambientLighting; }
  IsometricAmbientLighting& ambient() { return m_ambientLighting; }

  void setAmbientLighting(IsometricAmbientLighting ambient);
  void setAmbient(float ambient);
  void setAmbientDirection(glm::vec3 direction);
  void setAmbientColor(glm::vec3 color);
  void setAmbientDiffuseStrength(float strength);

private:
  void rebuildLightSnapshots();

private:
  struct PointLightCache
  {
    std::vector<IsometricPointLightSnapshot> lights;
    bool lightsDirty = true;
  };

  PointLightCache m_cache;
  IsometricAmbientLighting m_ambientLighting;
};

} // namespace sfs
