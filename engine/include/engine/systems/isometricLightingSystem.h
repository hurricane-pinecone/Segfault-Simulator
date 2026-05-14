#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/ecs/system.h"

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <vector>

namespace sfs
{

struct ActiveLight
{
  glm::vec2 worldPosition;
  float height = 0.0f;
  glm::vec3 color{1.0f};
  float intensity = 1.0f;
  float radius = 1.0f;
};

struct IsometricLightingSample
{
  glm::vec2 worldPosition;
  float elevationOffset = 0.0f;
};

struct IsometricComputedLighting
{
  glm::vec3 direction{0.0f, 0.0f, 1.0f};
  float intensity = 1.0f;
  float ambient = 0.18f;
  float diffuseStrength = 0.85f;
};

class IsometricLightingSystem : public System
{
public:
  explicit IsometricLightingSystem(AssetStore& assetStore);

  void rebuildLights();

  void setLightDirection(const glm::vec3& direction);
  void setLighting(float ambient, float diffuseStrength);

  IsometricComputedLighting
  computeLighting(const IsometricLightingSample& sample) const;

  float getAmbient() const { return m_ambient; }
  float getDiffuseStrength() const { return m_diffuseStrength; }

  const glm::vec3& getLightDirection() const { return m_lightDirection; }
  const std::vector<ActiveLight>& getLights() const { return lights; }

private:
  AssetStore& assetStore;

  std::vector<ActiveLight> lights;

  glm::vec3 m_lightDirection = {0.0f, 0.0f, 1.0f};

  float m_ambient = 0.18f;
  float m_diffuseStrength = 0.85f;
};

} // namespace sfs
