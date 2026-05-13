
#include "engine/systems/isometricLightingSystem.h"
#include "engine/components/lightEmitterComponent.h"
#include "engine/components/spriteComponent.h"
#include "engine/components/transformComponent.h"
#include "engine/ecs/registry.h"
#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace sfs
{

bool LitTextureKey::operator==(const LitTextureKey& other) const
{
  return spriteId == other.spriteId && normalSpriteId == other.normalSpriteId &&
         lightX == other.lightX && lightY == other.lightY &&
         lightZ == other.lightZ && intensity == other.intensity &&
         ambient == other.ambient && diffuse == other.diffuse;
}

std::size_t LitTextureKeyHash::operator()(const LitTextureKey& key) const
{
  std::size_t h = 17;
  h = h * 31 + std::hash<uint32_t>{}(key.spriteId);
  h = h * 31 + std::hash<uint32_t>{}(key.normalSpriteId);
  h = h * 31 + std::hash<int>{}(key.lightX);
  h = h * 31 + std::hash<int>{}(key.lightY);
  h = h * 31 + std::hash<int>{}(key.lightZ);
  h = h * 31 + std::hash<int>{}(key.intensity);
  h = h * 31 + std::hash<int>{}(key.ambient);
  h = h * 31 + std::hash<int>{}(key.diffuse);
  return h;
}

IsometricLightingSystem::IsometricLightingSystem(AssetStore& assetStore)
    : assetStore(assetStore)
{
  registerComponent<TransformComponent>();
  registerComponent<LightEmitterComponent>();
}

IsometricLightingSystem::~IsometricLightingSystem() { clearLitTextureCache(); }

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
  clearLitTextureCache();
}

void IsometricLightingSystem::setLighting(float ambient, float diffuseStrength)
{
  m_ambient = std::clamp(ambient, 0.0f, 1.0f);
  m_diffuseStrength = std::clamp(diffuseStrength, 0.0f, 2.0f);
  clearLitTextureCache();
}

bool IsometricLightingSystem::renderLitSprite(
    SDL_Renderer& renderer,
    const Entity& entity,
    const SpriteComponent& spriteComponent,
    const Sprite& sprite,
    const SDL_Rect& dest,
    const IsometricLightingSample& sample)
{
  if (!entity.hasComponent<NormalMapComponent>())
    return false;

  const auto& normalMap = entity.getComponent<NormalMapComponent>();
  const auto normalSprite = assetStore.getSprite(normalMap.spriteId);

  if (!normalSprite)
    return false;

  SDL_Surface* albedoSurface = assetStore.getSurface(sprite.textureId);
  SDL_Surface* normalSurface = assetStore.getSurface(normalSprite->textureId);

  if (!albedoSurface || !normalSurface)
    return false;

  const auto lighting = computeLighting(sample);

  constexpr int bucketScale = 6;
  constexpr int intensityScale = 3;
  constexpr int lightingScale = 8;

  LitTextureKey key{spriteComponent.spriteId,
                    normalMap.spriteId,
                    static_cast<int>(lighting.direction.x * bucketScale),
                    static_cast<int>(lighting.direction.y * bucketScale),
                    static_cast<int>(lighting.direction.z * bucketScale),
                    static_cast<int>(lighting.intensity * intensityScale),
                    static_cast<int>(m_ambient * lightingScale),
                    static_cast<int>(m_diffuseStrength * lightingScale)};

  SDL_Texture* litTexture = nullptr;

  auto it = litTextureCache.find(key);

  if (it != litTextureCache.end())
  {
    litTexture = it->second;
  }
  else
  {
    litTexture = createLitTexture(renderer,
                                  albedoSurface,
                                  normalSurface,
                                  sprite.srcRect,
                                  normalSprite->srcRect,
                                  lighting.direction,
                                  lighting.intensity);

    if (litTexture)
      litTextureCache.emplace(key, litTexture);
  }

  if (!litTexture)
    return false;

  SDL_RenderCopyEx(
      &renderer, litTexture, nullptr, &dest, 0.0, nullptr, SDL_FLIP_NONE);

  return true;
}

IsometricLightingSystem::ComputedLighting
IsometricLightingSystem::computeLighting(
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
    attenuation *= attenuation;

    glm::vec3 toLight{toLightWorld2.x,
                      toLightWorld2.y,
                      light.height - sample.elevationOffset};

    if (glm::length(toLight) > 0.001f)
    {
      accumulatedLightDir +=
          glm::normalize(toLight) * attenuation * light.intensity;

      accumulatedIntensity += attenuation * light.intensity;
    }
  }

  glm::vec3 lightDir =
      m_lightDirection * m_diffuseStrength + accumulatedLightDir;

  if (glm::length(lightDir) < 0.001f)
    lightDir = glm::vec3{0.0f, 0.0f, 1.0f};
  else
    lightDir = glm::normalize(lightDir);

  return {lightDir, std::clamp(1.0f + accumulatedIntensity, 0.0f, 2.5f)};
}

glm::vec3 IsometricLightingSystem::getFaceNormalFromColor(SDL_Color color) const
{
  glm::vec3 normal{color.r / 255.0f * 2.0f - 1.0f,
                   color.g / 255.0f * 2.0f - 1.0f,
                   color.b / 255.0f * 2.0f - 1.0f};

  if (glm::length(normal) < 0.001f)
    return glm::vec3{0.0f, 0.0f, 1.0f};

  return glm::normalize(normal);
}

float IsometricLightingSystem::computeBrightness(const glm::vec3& normal,
                                                 const glm::vec3& lightDir,
                                                 float intensity) const
{
  float ndotl = std::max(glm::dot(normal, lightDir), 0.0f);
  float diffuse = std::pow(ndotl, 0.75f);

  return std::clamp(m_ambient + diffuse * intensity, 0.0f, 1.0f);
}

SDL_Color
IsometricLightingSystem::getPixel(SDL_Surface* surface, int x, int y) const
{
  x = std::clamp(x, 0, surface->w - 1);
  y = std::clamp(y, 0, surface->h - 1);

  const int bpp = surface->format->BytesPerPixel;

  Uint8* p =
      static_cast<Uint8*>(surface->pixels) + y * surface->pitch + x * bpp;

  Uint32 pixel = 0;
  std::memcpy(&pixel, p, bpp);

  SDL_Color color;
  SDL_GetRGBA(pixel, surface->format, &color.r, &color.g, &color.b, &color.a);

  return color;
}

void IsometricLightingSystem::setPixel(SDL_Surface* surface,
                                       int x,
                                       int y,
                                       SDL_Color color) const
{
  x = std::clamp(x, 0, surface->w - 1);
  y = std::clamp(y, 0, surface->h - 1);

  const int bpp = surface->format->BytesPerPixel;

  Uint8* p =
      static_cast<Uint8*>(surface->pixels) + y * surface->pitch + x * bpp;

  Uint32 pixel =
      SDL_MapRGBA(surface->format, color.r, color.g, color.b, color.a);

  std::memcpy(p, &pixel, bpp);
}

SDL_Texture*
IsometricLightingSystem::createLitTexture(SDL_Renderer& renderer,
                                          SDL_Surface* albedoSurface,
                                          SDL_Surface* normalSurface,
                                          const SDL_Rect& albedoRect,
                                          const SDL_Rect& normalRect,
                                          const glm::vec3& lightDir,
                                          float lightIntensity)
{
  SDL_Surface* output = SDL_CreateRGBSurfaceWithFormat(
      0, albedoRect.w, albedoRect.h, 32, SDL_PIXELFORMAT_RGBA32);

  if (!output)
    return nullptr;

  SDL_LockSurface(albedoSurface);
  SDL_LockSurface(normalSurface);
  SDL_LockSurface(output);

  for (int y = 0; y < albedoRect.h; y++)
  {
    for (int x = 0; x < albedoRect.w; x++)
    {
      SDL_Color albedo =
          getPixel(albedoSurface, albedoRect.x + x, albedoRect.y + y);

      if (albedo.a == 0)
      {
        setPixel(output, x, y, SDL_Color{0, 0, 0, 0});
        continue;
      }

      SDL_Color normalColor =
          getPixel(normalSurface, normalRect.x + x, normalRect.y + y);

      glm::vec3 normal = getFaceNormalFromColor(normalColor);
      float brightness = computeBrightness(normal, lightDir, lightIntensity);

      SDL_Color out{
          static_cast<Uint8>(std::clamp(albedo.r * brightness, 0.0f, 255.0f)),
          static_cast<Uint8>(std::clamp(albedo.g * brightness, 0.0f, 255.0f)),
          static_cast<Uint8>(std::clamp(albedo.b * brightness, 0.0f, 255.0f)),
          albedo.a};

      setPixel(output, x, y, out);
    }
  }

  SDL_UnlockSurface(output);
  SDL_UnlockSurface(normalSurface);
  SDL_UnlockSurface(albedoSurface);

  SDL_Texture* litTexture = SDL_CreateTextureFromSurface(&renderer, output);
  SDL_FreeSurface(output);

  if (litTexture)
    SDL_SetTextureBlendMode(litTexture, SDL_BLENDMODE_BLEND);

  return litTexture;
}

void IsometricLightingSystem::clearLitTextureCache()
{
  for (auto& [key, texture] : litTextureCache)
  {
    if (texture)
      SDL_DestroyTexture(texture);
  }

  litTextureCache.clear();
}

} // namespace sfs
