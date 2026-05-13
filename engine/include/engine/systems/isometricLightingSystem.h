#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/components/spriteComponent.h"
#include "engine/ecs/system.h"

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <SDL2/SDL.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sfs
{

struct LitTextureKey
{
  uint32_t spriteId;
  uint32_t normalSpriteId;
  int lightX;
  int lightY;
  int lightZ;
  int intensity;
  int ambient;
  int diffuse;

  bool operator==(const LitTextureKey& other) const;
};

struct LitTextureKeyHash
{
  std::size_t operator()(const LitTextureKey& key) const;
};

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

class IsometricLightingSystem : public System
{
public:
  explicit IsometricLightingSystem(AssetStore& assetStore);
  ~IsometricLightingSystem();

  void rebuildLights();

  void setLightDirection(const glm::vec3& direction);
  void setLighting(float ambient, float diffuseStrength);

  bool renderLitSprite(SDL_Renderer& renderer,
                       const Entity& entity,
                       const SpriteComponent& spriteComponent,
                       const Sprite& sprite,
                       const SDL_Rect& dest,
                       const IsometricLightingSample& sample);

private:
  struct ComputedLighting
  {
    glm::vec3 direction{0.0f, 0.0f, 1.0f};
    float intensity = 1.0f;
  };

  ComputedLighting computeLighting(const IsometricLightingSample& sample) const;

  glm::vec3 getFaceNormalFromColor(SDL_Color color) const;

  float computeBrightness(const glm::vec3& normal,
                          const glm::vec3& lightDir,
                          float intensity) const;

  SDL_Color getPixel(SDL_Surface* surface, int x, int y) const;
  void setPixel(SDL_Surface* surface, int x, int y, SDL_Color color) const;

  SDL_Texture* createLitTexture(SDL_Renderer& renderer,
                                SDL_Surface* albedoSurface,
                                SDL_Surface* normalSurface,
                                const SDL_Rect& albedoRect,
                                const SDL_Rect& normalRect,
                                const glm::vec3& lightDir,
                                float lightIntensity);

  void clearLitTextureCache();

private:
  AssetStore& assetStore;

  std::vector<ActiveLight> lights;

  std::unordered_map<LitTextureKey, SDL_Texture*, LitTextureKeyHash>
      litTextureCache;

  glm::vec3 m_lightDirection = {0.0f, 0.0f, 1.0f};

  float m_ambient = 0.18f;
  float m_diffuseStrength = 0.85f;
};

} // namespace sfs
