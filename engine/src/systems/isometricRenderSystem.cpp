#include "engine/systems/isometricRenderSystem.h"

#include "engine/components/cameraComponent.h"
#include "engine/components/spriteComponent.h"
#include "engine/ecs/registry.h"
#include "engine/logger/logger.h"
#include "engine/renderers/renderContext.h"
#include "engine/systems/cameraSystem.h"
#include "engine/systems/isometricLightingSystem.h"
#include "glm/glm/ext/vector_float2.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

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

  const auto& activeCamera = getCamera();
  auto cameraPosition = getCameraPosition();
  float zoom = activeCamera.camera ? activeCamera.camera->zoom : 1;

  const glm::vec2 screenCenter{
      static_cast<float>(windowWidth) / 2.0f,
      static_cast<float>(windowHeight) / 2.0f,
  };

  const glm::vec2 isoCameraPosition = gridToIsometric(cameraPosition);

  std::unordered_set<CellKey, CellKeyHash> occupiedCells;

  for (const auto& entity : getEntities())
  {
    if (isTileEntity(entity))
      continue;

    const auto& transform = entity.getComponent<TransformComponent>();

    const glm::ivec2 cell = gridCellOf(transform.position);
    const int elevation = getTileElevationAt(glm::vec2{cell});

    occupiedCells.insert(CellKey{cell.x, cell.y, elevation});
  }

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
        (isoPosition - isoCameraPosition) * zoom + screenCenter;

    const int width = static_cast<int>(sprite->srcRect.w * transform.scale.x *
                                       worldScale * zoom);

    const int height = static_cast<int>(sprite->srcRect.h * transform.scale.y *
                                        worldScale * zoom);

    const float anchorX = spriteComponent.anchor.x * static_cast<float>(width);
    const float anchorY = spriteComponent.anchor.y * static_cast<float>(height);

    const glm::vec2 groundSamplePosition =
        getGroundSamplePosition(entity, transform);

    const int elevationLevel =
        getRenderElevationLevel(entity, groundSamplePosition);

    const int elevationOffset = static_cast<int>(
        std::round(elevationLevel * elevationStep * worldScale * zoom));

    const float waveOffset = getWaveOffset(groundSamplePosition) * zoom;

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

    RenderItem item;

    const bool tileEntity = isTileEntity(entity);

    glm::vec2 exactPosition = transform.position;
    glm::vec2 sortPosition = exactPosition;

    if (!tileEntity)
    {
      const glm::ivec2 cell = gridCellOf(exactPosition);

      sortPosition = glm::vec2{
          static_cast<float>(cell.x) + 0.5f,
          static_cast<float>(cell.y) + 0.5f,
      };
    }

    constexpr float ElevationSortWeight = 0.5f;
    constexpr float SpriteBias = 0.001f;

    float sortKey = sortPosition.x + sortPosition.y +
                    static_cast<float>(elevationLevel) * ElevationSortWeight;

    glm::vec2 spriteWorldSample =
        tileEntity ? transform.position + glm::vec2{0.5f, 0.5f}
                   : transform.position;

    if (tileEntity)
    {
      item.worldPoints[0] = transform.position;
      item.worldPoints[1] = transform.position + glm::vec2{1.0f, 0.0f};
      item.worldPoints[2] = transform.position + glm::vec2{1.0f, 1.0f};
      item.worldPoints[3] = transform.position + glm::vec2{0.0f, 1.0f};
    }
    else
    {
      // Tiny exact-position tie-break inside the tile.
      const glm::vec2 fractional{
          exactPosition.x - std::floor(exactPosition.x),
          exactPosition.y - std::floor(exactPosition.y),
      };

      const float exactTieBreak =
          (fractional.x + fractional.y - 1.0f) * 0.0001f;

      sortKey += SpriteBias;
      sortKey += exactTieBreak;

      item.worldPoints[0] = spriteWorldSample;
      item.worldPoints[1] = spriteWorldSample;
      item.worldPoints[2] = spriteWorldSample;
      item.worldPoints[3] = spriteWorldSample;
    }

    item.textureId = &sprite->textureId;
    item.srcRect = sprite->srcRect;
    item.dest = dest;
    item.textureWidth = surface->w;
    item.textureHeight = surface->h;
    item.sortKey = sortKey;
    item.tint = SDL_Color{255, 255, 255, 255};
    item.renderLayer = tileEntity ? 0 : 2;

    if (lightingSystem)
    {
      item.lighting = lightingSystem->computeLighting({
          spriteWorldSample,
          static_cast<float>(elevationLevel),
      });

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
            item.normalTextureId = &normalSprite->textureId;
            item.normalSrcRect = normalSprite->srcRect;
            item.normalTextureWidth = normalSurface->w;
            item.normalTextureHeight = normalSurface->h;
          }
        }
      }
    }

    if (lightingSystem && !isTileEntity(entity))
    {
      const auto shadows =
          computeIsometricShadows(lightingSystem->getLights(),
                                  lightingSystem->getLightDirection(),
                                  lightingSystem->getDiffuseStrength(),
                                  spriteWorldSample,
                                  item.dest.h,
                                  worldScale,
                                  tileWidth,
                                  tileHeight);

      for (const auto& shadow : shadows)
        submitShadow(item, shadow.offset, shadow.alpha);
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
  shadow.normalTextureId = nullptr;
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
                     if (a.sortKey < b.sortKey)
                       return true;

                     if (a.sortKey > b.sortKey)
                       return false;

                     return a.renderLayer < b.renderLayer;
                   });

  for (const auto& item : renderItems)
  {
    auto& quadRenderer = RenderContext::quadRenderer();
    SDL_Surface* surface = assetStore.getSurface(*item.textureId);

    if (!surface)
      continue;

    const unsigned int texture =
        quadRenderer.getOrCreateTexture(*item.textureId, surface);

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

      command.lightDirection = item.lighting.direction;
      command.lightIntensity = item.lighting.intensity;
      command.ambient = item.lighting.ambient;
      command.diffuseStrength = item.lighting.diffuseStrength;
      command.lightColor = item.lighting.color;
      command.worldPoints[0] = item.worldPoints[0];
      command.worldPoints[1] = item.worldPoints[1];
      command.worldPoints[2] = item.worldPoints[2];
      command.worldPoints[3] = item.worldPoints[3];

      if (registry->hasSystem<IsometricLightingSystem>())
      {
        const auto& lightingSystem =
            registry->getSystem<IsometricLightingSystem>();
        const auto& lights = lightingSystem.getLights();

        command.lightCount = std::min(static_cast<int>(lights.size()),
                                      OpenGLQuadRenderer::MaxShaderLights);

        for (int i = 0; i < command.lightCount; i++)
        {
          command.lightPositions[i] = lights[i].worldPosition;
          command.lightColors[i] = lights[i].color;
          command.lightIntensities[i] = lights[i].intensity;
          command.lightRadii[i] = lights[i].radius;
          command.lightHeights[i] = lights[i].height;
        }
      }

      if (item.hasNormalMap)
      {
        SDL_Surface* normalSurface =
            assetStore.getSurface(*item.normalTextureId);

        if (normalSurface)
        {
          command.normalTexture = quadRenderer.getOrCreateTexture(
              *item.normalTextureId, normalSurface);

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
  const auto& activeCamera = getCamera();
  float zoom = activeCamera.camera ? activeCamera.camera->zoom : 1;
  auto& quadRenderer = RenderContext::quadRenderer();

  const glm::vec2 cameraPosition = getCameraPosition();

  const glm::vec2 screenCenter{
      static_cast<float>(windowWidth) / 2.0f,
      static_cast<float>(windowHeight) / 2.0f,
  };

  glm::vec2 screenPosition =
      (gridToIsometric(gridPosition) - gridToIsometric(cameraPosition)) * zoom +
      screenCenter;

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

ActiveCamera IsometricRenderSystem::getCamera() const
{
  if (!registry->hasSystem<CameraSystem>())
    return {};

  const auto& cameras = registry->getSystem<CameraSystem>().getEntities();

  if (cameras.empty())
    return {};

  const auto& cameraEntity = cameras.front();

  return {
      &cameraEntity.getComponent<CameraComponent>(),
      &cameraEntity.getComponent<TransformComponent>(),
  };
}

glm::vec2 IsometricRenderSystem::getCameraPosition() const
{
  const auto activeCamera = getCamera();

  if (!activeCamera.camera || !activeCamera.transform)
    return {0.0f, 0.0f};

  return activeCamera.transform->position + activeCamera.camera->offset;
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
