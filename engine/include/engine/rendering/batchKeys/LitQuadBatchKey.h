#pragma once

#include "engine/rendering/quads.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

struct LitBatchKey
{
  unsigned int texture = 0;
  unsigned int normalTexture = 0;
  bool hasNormalMap = false;

  glm::vec3 lightDirection{0.0f};
  float lightIntensity = 1.0f;
  float ambient = 0.0f;
  float diffuseStrength = 0.0f;
  glm::vec3 lightColor{1.0f};

  int surfaceEffect = 0;

  static LitBatchKey from(const LitQuad& command,
                          unsigned int defaultNormalTexture)
  {
    LitBatchKey key;

    key.texture = command.texture;

    key.normalTexture = command.hasNormalMap && command.normalTexture != 0
                            ? command.normalTexture
                            : defaultNormalTexture;

    key.hasNormalMap = command.hasNormalMap && command.normalTexture != 0;

    key.lightDirection = command.lightDirection;
    key.lightIntensity = command.lightIntensity;
    key.ambient = command.ambient;
    key.diffuseStrength = command.diffuseStrength;
    key.lightColor = command.lightColor;

    key.surfaceEffect = command.surfaceEffect;

    return key;
  }

  bool operator==(const LitBatchKey& other) const
  {
    if (texture != other.texture)
      return false;

    if (normalTexture != other.normalTexture)
      return false;

    if (hasNormalMap != other.hasNormalMap)
      return false;

    if (lightDirection != other.lightDirection)
      return false;

    if (lightIntensity != other.lightIntensity)
      return false;

    if (ambient != other.ambient)
      return false;

    if (diffuseStrength != other.diffuseStrength)
      return false;

    if (lightColor != other.lightColor)
      return false;

    if (surfaceEffect != other.surfaceEffect)
      return false;

    return true;
  }
};
} // namespace sfs
