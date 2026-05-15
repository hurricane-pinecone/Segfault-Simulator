#include "engine/systems/isometricRenderSystem.h"

#include "engine/components/spriteComponent.h"
#include "engine/ecs/registry.h"
#include "engine/logger/logger.h"
#include "engine/renderers/renderContext.h"
#include "engine/systems/cameraSystem.h"
#include "engine/systems/isometricLightingSystem.h"

#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>

namespace sfs
{

ElevationComponent::ElevationComponent(int level) : level(level) {}

IsometricRenderSystem::IsometricRenderSystem(AssetStore& assetStore,
                                             int windowWidth,
                                             int windowHeight,
                                             int tileWidth,
                                             int tileHeight)
    : assetStore(assetStore), windowWidth(windowWidth),
      windowHeight(windowHeight), tileWidth(tileWidth), tileHeight(tileHeight)
{
  registerComponent<SpriteComponent>();
  registerComponent<TransformComponent>();
}

IsometricRenderSystem::~IsometricRenderSystem() = default;

void IsometricRenderSystem::render()
{
  beginBatches();

  auto* lightingSystem = registry->hasSystem<IsometricLightingSystem>()
                             ? &registry->getSystem<IsometricLightingSystem>()
                             : nullptr;

  if (lightingSystem)
    lightingSystem->rebuildLights();

  const glm::vec2 cameraPosition = getCameraPosition();

  const glm::vec2 screenCenter{
      static_cast<float>(windowWidth) / 2.0f,
      static_cast<float>(windowHeight) / 2.0f,
  };

  const glm::vec2 isoCameraPosition = gridToIsometric(cameraPosition);

  for (const auto& entity : getEntities())
  {
    const auto& transform = entity.getComponent<TransformComponent>();
    const auto& spriteComponent = entity.getComponent<SpriteComponent>();

    const auto sprite = assetStore.getSprite(spriteComponent.spriteId);

    if (!sprite)
    {
      LOG_ERROR("Attempted to render NULL isometric sprite");
      continue;
    }

    SDL_Surface* surface = assetStore.getSurface(sprite->textureId);

    if (!surface)
    {
      LOG_ERROR("NULL surface included in OpenGL isometric render loop");
      continue;
    }

    const glm::vec2 isoPosition = gridToIsometric(transform.position);

    const glm::vec2 screenPosition =
        isoPosition - isoCameraPosition + screenCenter;

    const int width =
        static_cast<int>(sprite->srcRect.w * transform.scale.x * worldScale);

    const int height =
        static_cast<int>(sprite->srcRect.h * transform.scale.y * worldScale);

    const float anchorX = spriteComponent.anchor.x * static_cast<float>(width);
    const float anchorY = spriteComponent.anchor.y * static_cast<float>(height);

    const glm::vec2 groundSamplePosition =
        getGroundSamplePosition(entity, transform);

    const int elevationLevel =
        getRenderElevationLevel(entity, groundSamplePosition);

    const int elevationOffset = static_cast<int>(
        std::round(elevationLevel * elevationStep * worldScale));

    const float waveOffset = getWaveOffset(groundSamplePosition);

    const glm::vec2 surfacePosition{
        screenPosition.x,
        screenPosition.y - elevationOffset - waveOffset,
    };

    SDL_Rect dest{
        static_cast<int>(std::round(surfacePosition.x - anchorX)),
        static_cast<int>(std::round(surfacePosition.y - anchorY)),
        width,
        height,
    };

    float sortKey = transform.position.x + transform.position.y;

    if (isTileEntity(entity))
    {
      sortKey += static_cast<float>(elevationLevel) * 0.01f;
    }
    else
    {
      sortKey += static_cast<float>(elevationLevel) * 0.01f;
      sortKey += 0.005f;
    }

    RenderItem item;
    item.textureId = sprite->textureId;
    item.srcRect = sprite->srcRect;
    item.dest = dest;
    item.textureWidth = surface->w;
    item.textureHeight = surface->h;
    item.sortKey = sortKey;
    item.tint = SDL_Color{255, 255, 255, 255};
    item.renderLayer = isTileEntity(entity) ? 0 : 2;

    glm::vec2 spriteWorldSample =
        isTileEntity(entity) ? transform.position + glm::vec2{0.5f, 0.5f}
                             : transform.position;

    if (lightingSystem)
    {
      IsometricLightingSample lightingSample{
          spriteWorldSample,
          static_cast<float>(elevationOffset),
      };

      const auto lighting = lightingSystem->computeLighting(lightingSample);

      item.lightDirection = lighting.direction;
      item.lightIntensity = lighting.intensity;
      item.ambient = lighting.ambient;
      item.diffuseStrength = lighting.diffuseStrength;
    }

    if (entity.hasComponent<NormalMapComponent>())
    {
      const auto& normalMap = entity.getComponent<NormalMapComponent>();
      const auto normalSprite = assetStore.getSprite(normalMap.spriteId);

      if (normalSprite)
      {
        SDL_Surface* normalSurface =
            assetStore.getSurface(normalSprite->textureId);

        if (normalSurface)
        {
          item.hasNormalMap = true;
          item.normalTextureId = normalSprite->textureId;
          item.normalSrcRect = normalSprite->srcRect;
          item.normalTextureWidth = normalSurface->w;
          item.normalTextureHeight = normalSurface->h;
        }
      }
    }

    if (lightingSystem && !isTileEntity(entity))
    {
      const auto& lights = lightingSystem->getLights();

      for (const auto& light : lights)
      {
        glm::vec2 delta = spriteWorldSample - light.worldPosition;
        float distance = glm::length(delta);

        if (distance > light.radius || distance < 0.001f)
          continue;

        glm::vec2 shadowWorldDir = delta / distance;

        float attenuation = 1.0f - distance / light.radius;
        attenuation = std::pow(std::clamp(attenuation, 0.0f, 1.0f), 0.75f);

        float casterHeight = static_cast<float>(item.dest.h) * 0.75f;
        float visualLightHeight = std::max(light.height * 0.0625f, 1.0f);
        float shadowLength = casterHeight * distance / visualLightHeight;

        glm::vec2 shadowOffset =
            worldDirToShadowOffset(shadowWorldDir, shadowLength);

        float alpha =
            0.42f * attenuation * std::clamp(light.intensity, 0.0f, 2.0f);

        submitShadow(item, shadowOffset, alpha);
      }

      glm::vec3 sunDir = lightingSystem->getLightDirection();
      float horizonFade = glm::smoothstep(0.0f, 0.15f, sunDir.z);

      if (lightingSystem->getDiffuseStrength() > 0.001f && horizonFade > 0.001f)
      {
        glm::vec2 sunHorizontal{sunDir.x, sunDir.y};
        float horizontalLength = glm::length(sunHorizontal);

        if (horizontalLength > 0.001f)
        {
          glm::vec2 shadowWorldDir = -sunHorizontal / horizontalLength;

          glm::vec2 isoDir =
              gridToIsometric(shadowWorldDir) - gridToIsometric({0.0f, 0.0f});

          if (glm::length(isoDir) > 0.001f)
          {
            isoDir = glm::normalize(isoDir);

            float noonFade = glm::smoothstep(0.0f, 0.25f, horizontalLength);

            float lowSunStretch = std::clamp(
                horizontalLength / std::max(sunDir.z, 0.03f), 0.0f, 12.0f);

            float shadowLength = static_cast<float>(item.dest.h) *
                                 lowSunStretch * 0.45f * noonFade;

            float shadowAlpha = 0.22f * noonFade * horizonFade;

            if (shadowLength > 0.5f && shadowAlpha > 0.01f)
              submitShadow(item, isoDir * shadowLength, shadowAlpha);
          }
        }
      }
    }

    submitSprite(item);
  }

  flushBatches();
}

void IsometricRenderSystem::beginBatches() { renderItems.clear(); }

void IsometricRenderSystem::submitSprite(const RenderItem& item)
{
  renderItems.push_back(item);
}

void IsometricRenderSystem::submitShadow(const RenderItem& caster,
                                         const glm::vec2& shadowOffset,
                                         float alpha,
                                         float sortKeyBias)
{
  RenderItem shadow = caster;

  shadow.isShadow = true;
  shadow.hasNormalMap = false;
  shadow.normalTextureId.clear();
  shadow.shadowOffset = shadowOffset;
  shadow.renderLayer = 1;

  const Uint8 a = static_cast<Uint8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);

  shadow.tint = SDL_Color{0, 0, 0, a};
  shadow.sortKey = caster.sortKey + sortKeyBias;

  renderItems.push_back(shadow);
}

void IsometricRenderSystem::flushBatches()
{
  std::stable_sort(renderItems.begin(),
                   renderItems.end(),
                   [](const RenderItem& a, const RenderItem& b)
                   {
                     if (a.renderLayer != b.renderLayer)
                       return a.renderLayer < b.renderLayer;

                     return a.sortKey < b.sortKey;
                   });

  for (const auto& item : renderItems)
  {
    auto& quadRenderer = RenderContext::quadRenderer();
    SDL_Surface* surface = assetStore.getSurface(item.textureId);

    if (!surface)
      continue;

    const unsigned int texture =
        quadRenderer.getOrCreateTexture(item.textureId, surface);

    if (texture == 0)
      continue;

    if (item.isShadow)
    {
      const float left = static_cast<float>(item.dest.x);
      const float right = static_cast<float>(item.dest.x + item.dest.w);
      const float bottom = static_cast<float>(item.dest.y + item.dest.h);

      OpenGLQuadRenderer::FreeformQuadDrawCommand command;

      command.texture = texture;
      command.srcRect = item.srcRect;
      command.textureWidth = item.textureWidth;
      command.textureHeight = item.textureHeight;
      command.tint = item.tint;

      command.points[0] = glm::vec2{left, bottom} + item.shadowOffset;
      command.points[1] = glm::vec2{right, bottom} + item.shadowOffset;
      command.points[2] = glm::vec2{right, bottom};
      command.points[3] = glm::vec2{left, bottom};

      quadRenderer.drawFreeformQuad(command);
    }
    else
    {
      OpenGLQuadRenderer::LitQuadDrawCommand command;

      command.texture = texture;
      command.srcRect = item.srcRect;
      command.destRect = item.dest;
      command.textureWidth = item.textureWidth;
      command.textureHeight = item.textureHeight;
      command.tint = item.tint;

      command.lightDirection = item.lightDirection;
      command.lightIntensity = item.lightIntensity;
      command.ambient = item.ambient;
      command.diffuseStrength = item.diffuseStrength;

      if (item.hasNormalMap)
      {
        SDL_Surface* normalSurface =
            assetStore.getSurface(item.normalTextureId);

        if (normalSurface)
        {
          command.normalTexture = quadRenderer.getOrCreateTexture(
              item.normalTextureId, normalSurface);

          command.hasNormalMap = command.normalTexture != 0;
        }
      }

      quadRenderer.drawLitQuad(command);
    }
  }
}

void IsometricRenderSystem::setWaveTime(float time) { waveTime = time; }

void IsometricRenderSystem::setWaveEnabled(bool enabled)
{
  waveEnabled = enabled;
}

void IsometricRenderSystem::drawDebugTile(const glm::vec2& gridPosition,
                                          SDL_Color color)
{
  drawDebugTile(gridPosition, getTileElevationAt(gridPosition), color);
}

void IsometricRenderSystem::drawDebugTile(const glm::vec2& gridPosition,
                                          int elevation,
                                          SDL_Color color)
{
  auto& quadRenderer = RenderContext::quadRenderer();

  const glm::vec2 cameraPosition = getCameraPosition();

  const glm::vec2 screenCenter{
      static_cast<float>(windowWidth) / 2.0f,
      static_cast<float>(windowHeight) / 2.0f,
  };

  glm::vec2 screenPosition = gridToIsometric(gridPosition) -
                             gridToIsometric(cameraPosition) + screenCenter;

  screenPosition.y -= elevation * elevationStep;
  screenPosition.y -= getWaveOffset(gridPosition);

  glm::vec2 points[5] = {
      {screenPosition.x, screenPosition.y},
      {screenPosition.x + tileWidth / 2.0f,
       screenPosition.y + tileHeight / 2.0f},
      {screenPosition.x, screenPosition.y + tileHeight},
      {screenPosition.x - tileWidth / 2.0f,
       screenPosition.y + tileHeight / 2.0f},
      {screenPosition.x, screenPosition.y},
  };

  quadRenderer.drawLineLoop(points, 5, color);
}

void IsometricRenderSystem::setLightDirection(const glm::vec3& direction)
{
  if (!registry->hasSystem<IsometricLightingSystem>())
    return;

  registry->getSystem<IsometricLightingSystem>().setLightDirection(direction);
}

void IsometricRenderSystem::setLighting(float ambient, float diffuseStrength)
{
  if (!registry->hasSystem<IsometricLightingSystem>())
    return;

  registry->getSystem<IsometricLightingSystem>().setLighting(
      ambient, diffuseStrength);
}

glm::vec2
IsometricRenderSystem::gridToIsometric(const glm::vec2& gridPosition) const
{
  return {
      (gridPosition.x - gridPosition.y) *
          (static_cast<float>(tileWidth) * worldScale) / 2.0f,

      (gridPosition.x + gridPosition.y) *
          (static_cast<float>(tileHeight) * worldScale) / 2.0f,
  };
}

glm::vec2
IsometricRenderSystem::worldDirToShadowOffset(const glm::vec2& worldDir,
                                              float length) const
{
  glm::vec2 isoDir =
      gridToIsometric(worldDir) - gridToIsometric(glm::vec2{0.0f, 0.0f});

  if (glm::length(isoDir) < 0.001f)
    return {0.0f, 0.0f};

  return glm::normalize(isoDir) * length;
}

glm::vec2 IsometricRenderSystem::isometricToGrid(const glm::vec2& iso) const
{
  const float scaledTileWidth = static_cast<float>(tileWidth) * worldScale;
  const float scaledTileHeight = static_cast<float>(tileHeight) * worldScale;

  float x =
      (iso.x / (scaledTileWidth / 2.0f) + iso.y / (scaledTileHeight / 2.0f)) *
      0.5f;

  float y =
      (iso.y / (scaledTileHeight / 2.0f) - iso.x / (scaledTileWidth / 2.0f)) *
      0.5f;

  return {x, y};
}

glm::vec2 IsometricRenderSystem::getCameraPosition() const
{
  if (!registry->hasSystem<CameraSystem>())
    return {0.0f, 0.0f};

  const auto& camera = registry->getSystem<CameraSystem>().getEntities();

  if (camera.empty())
    return {0.0f, 0.0f};

  return camera[0].getComponent<TransformComponent>().position;
}

glm::ivec2 IsometricRenderSystem::gridCellOf(const glm::vec2& position) const
{
  return {
      static_cast<int>(std::floor(position.x)),
      static_cast<int>(std::floor(position.y)),
  };
}

bool IsometricRenderSystem::isTileEntity(const Entity& entity) const
{
  return entity.hasComponent<IsometricTile>();
}

int IsometricRenderSystem::getRenderElevationLevel(
    const Entity& entity,
    const glm::vec2& samplePosition) const
{
  if (isTileEntity(entity))
  {
    if (entity.hasComponent<ElevationComponent>())
      return entity.getComponent<ElevationComponent>().level;

    return 0;
  }

  return getTileElevationAt(samplePosition);
}

glm::vec2 IsometricRenderSystem::getGroundSamplePosition(
    const Entity& entity,
    const TransformComponent& transform) const
{
  if (isTileEntity(entity))
    return transform.position;

  return glm::floor(transform.position);
}

bool IsometricRenderSystem::tryGetTileElevationAt(const glm::vec2& position,
                                                  int& outElevation) const
{
  const glm::ivec2 targetTile = gridCellOf(position);

  bool found = false;
  int highest = 0;

  for (const auto& entity : getEntities())
  {
    if (!isTileEntity(entity))
      continue;

    if (!entity.hasComponent<ElevationComponent>())
      continue;

    const auto& transform = entity.getComponent<TransformComponent>();

    if (gridCellOf(transform.position) != targetTile)
      continue;

    const auto& elevation = entity.getComponent<ElevationComponent>();

    highest = std::max(highest, elevation.level);
    found = true;
  }

  outElevation = highest;
  return found;
}

int IsometricRenderSystem::getTileElevationAt(const glm::vec2& position) const
{
  int elevation = 0;
  tryGetTileElevationAt(position, elevation);
  return elevation;
}

float IsometricRenderSystem::getWaveOffset(const glm::vec2& gridPosition) const
{
  if (!waveEnabled)
    return 0.0f;

  return std::sin(gridPosition.x * waveFrequency +
                  gridPosition.y * waveFrequency + waveTime * waveSpeed) *
         waveAmplitude;
}

void IsometricRenderSystem::setWorldScale(float scale)
{
  worldScale = std::max(scale, 1.0f);
}

float IsometricRenderSystem::getWorldScale() const { return worldScale; }

} // namespace sfs
