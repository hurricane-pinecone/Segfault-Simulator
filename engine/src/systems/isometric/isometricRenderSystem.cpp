
#include "engine/assetStore/assetStore.h"
#include "engine/components/cameraComponent.h"
#include "engine/components/elevationComponent.h"
#include "engine/components/spriteComponent.h"

#include "engine/components/tags/isometricTile.h"
#include "engine/renderers/isometricRenderContext.h"
#include "engine/renderers/renderContext.h"
#include "engine/systems/isometric/isometricRenderSystem.h"

#include "engine/ecs/registry.h"
#include "engine/logger/logger.h"

#include "engine/systems/cameraSystem.h"
#include "engine/systems/isometric/isometricLightingSystem.h"
#include "engine/systems/isometric/isometricShadowSystem.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include <cstddef>

namespace sfs
{

int gTerrainShadowItems = 0;
int gTerrainShadowFlushes = 0;
int gRenderItemCount = 0;
int gTerrainShadowEdgesProcessed = 0;
int gTileRenderItems = 0;
int gSpriteRenderItems = 0;
int gSpriteProjectedShadowItems = 0;

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
  // Debug watch
  gTerrainShadowItems = 0;
  gTerrainShadowFlushes = 0;
  gRenderItemCount = 0;
  gTerrainShadowEdgesProcessed = 0;
  gTileRenderItems = 0;
  gSpriteRenderItems = 0;
  gSpriteProjectedShadowItems = 0;

  beginBatches();
  if (tileElevationCacheDirty)
  {
    rebuildTileElevationCache();
    tileElevationCacheDirty = false;
  }

  const auto lightingSystem =
      registry->hasSystem<IsometricLightingSystem>()
          ? &registry->getSystem<IsometricLightingSystem>()
          : nullptr;
  const auto ambientLighting =
      lightingSystem ? &lightingSystem->ambient() : nullptr;
  const auto pointLights =
      lightingSystem ? &lightingSystem->getPointLights() : nullptr;

  const auto& activeCamera = getCamera();
  auto cameraPosition = getCameraPosition();
  float zoom = activeCamera.camera ? activeCamera.camera->zoom : 1;

  const glm::vec2 screenCenter{
      static_cast<float>(windowWidth) / 2.0f,
      static_cast<float>(windowHeight) / 2.0f,
  };

  const glm::vec2 isoCameraPosition = gridToIsometric(cameraPosition);

  m_context.windowWidth = windowWidth;
  m_context.windowHeight = windowHeight;
  m_context.tileWidth = tileWidth;
  m_context.tileHeight = tileHeight;
  m_context.elevationStep = elevationStep;
  m_context.worldScale = worldScale;
  m_context.zoom = zoom;
  m_context.screenCenter = {
      static_cast<float>(windowWidth) * 0.5f,
      static_cast<float>(windowHeight) * 0.5f,
  };
  m_context.isoCameraPosition = isoCameraPosition;
  m_context.waveEnabled = waveEnabled;
  m_context.waveTime = waveTime;
  m_context.waveAmplitude = waveAmplitude;
  m_context.waveFrequency = waveFrequency;
  m_context.waveSpeed = waveSpeed;
  m_context.tileElevations = &tileElevationCache;

  if (ambientLighting)
    lightingSystem->submitLighting(m_context, m_renderQueue);

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

    // Rendering position stays exact. Anchor, elevation, wave motion, camera,
    // and zoom all affect the final screen rect, but should not directly decide
    // depth ordering.
    const glm::vec2 isoPosition = gridToIsometric(transform.position);

    const glm::vec2 screenPosition =
        (isoPosition - isoCameraPosition) * zoom + screenCenter;

    const int width = static_cast<int>(sprite->srcRect.w * transform.scale.x *
                                       worldScale * zoom);

    const int height = static_cast<int>(sprite->srcRect.h * transform.scale.y *
                                        worldScale * zoom);

    // Anchors define how the sprite image attaches to its world position.
    // Blocks can use a top anchor while actors use a feet/bottom anchor.
    // Sorting below does not assume every sprite has the same anchor.
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

    IsometricRenderItem item;

    const bool tileEntity = isTileEntity(entity);

    // exactPosition is where the entity really is in world/grid space.
    // It is still used for rendering, lighting, and fractional tie-breaks.
    glm::vec2 exactPosition = transform.position;

    // sortPosition is intentionally different for non-tile sprites.
    //
    // Moving actors use fractional positions, which made them flip in front of
    // nearby raised blocks just before stepping onto them. For depth ordering,
    // sprites are grouped by the center of their current grid cell. Their exact
    // fractional position is added later only as a tiny tie-break.
    glm::vec2 sortPosition = exactPosition;

    if (!tileEntity)
    {
      const glm::ivec2 cell = gridCellOf(exactPosition);

      sortPosition = glm::vec2{
          static_cast<float>(cell.x) + 0.5f,
          static_cast<float>(cell.y) + 0.5f,
      };
    }

    // Elevation contributes to sorting, but only partially.
    //
    // 0.01 was too weak: raised blocks failed to occlude actors behind them.
    // 1.0 was too strong: raised blocks behind the actor could incorrectly draw
    // over the actor. 0.5 matches the visual half-tile rise of this isometric
    // projection closely enough for stable painter ordering.
    constexpr float ElevationSortWeight = 0.5f;

    // Non-tile sprites sort just after tiles at the same effective depth, so an
    // actor standing on a tile appears above that tile instead of underneath
    // it.
    constexpr float SpriteBias = 0.001f;

    float sortKey = sortPosition.x + sortPosition.y +
                    static_cast<float>(elevationLevel) * ElevationSortWeight;

    // Tiles use the tile center for lighting because their lit surface spans
    // the whole tile. Sprites use their exact world position, which represents
    // their feet/contact point for actors and the authored position for props.
    glm::vec2 spriteWorldSample =
        tileEntity ? transform.position + glm::vec2{0.5f, 0.5f}
                   : transform.position;

    item.textureId = &sprite->textureId;
    item.srcRect = sprite->srcRect;
    item.dest = dest;
    item.textureWidth = surface->w;
    item.textureHeight = surface->h;
    item.sortKey = sortKey;
    item.tint = SDL_Color{255, 255, 255, 255};
    item.renderLayer = tileEntity ? 0 : 2;
    item.screenSortY = static_cast<float>(dest.y + dest.h);

    if (tileEntity)
    {
      item.worldPoints[0] = transform.position;
      item.worldPoints[1] = transform.position + glm::vec2{1.0f, 0.0f};
      item.worldPoints[2] = transform.position + glm::vec2{1.0f, 1.0f};
      item.worldPoints[3] = transform.position + glm::vec2{0.0f, 1.0f};
    }
    else
    {
      // Tiny exact-position tie-break inside the current tile.
      //
      // The main sprite sort uses the tile center for stability against raised
      // neighboring blocks. This tie-break restores local ordering between
      // sprites/props inside the same tile, such as a player and a lamp both
      // placed around the tile center.
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

    // Ambient lighting
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

    m_renderQueue.submit(item);

    if (!tileEntity && registry->hasSystem<IsometricShadowSystem>())
    {
      registry->getSystem<IsometricShadowSystem>().submitSpriteShadow(
          m_context,
          item,
          spriteWorldSample,
          elevationLevel,
          ambientLighting,
          pointLights,
          m_renderQueue);
    }
  }

  if (lightingSystem && registry->hasSystem<IsometricShadowSystem>())
  {
    registry->getSystem<IsometricShadowSystem>().submitTerrainEdgeShadows(
        m_context, *ambientLighting, m_renderQueue);
  }

  flushBatches();
}

void IsometricRenderSystem::beginBatches() { m_renderQueue.clear(); }

void IsometricRenderSystem::flushBatches()
{

  auto& quadRenderer = RenderContext::quadRenderer();
  auto& items = m_renderQueue.mutableItems();

  quadRenderer.beginSolidQuads();

  // Debug
  gRenderItemCount = static_cast<int>(m_renderQueue.size());

  for (const IsometricRenderItem& item : items)
  {
    if (item.isTerrainShadow)
      gTerrainShadowItems++;
    else if (item.renderLayer == 0)
      gTileRenderItems++;
    else
      gSpriteRenderItems++;
  }

  std::stable_sort(
      items.begin(),
      items.end(),
      [](const IsometricRenderItem& a, const IsometricRenderItem& b)
      {
        // Depth must be primary. Earlier versions sorted by
        // renderLayer first, which forced every tile behind every
        // sprite and made raised/closer blocks unable to occlude
        // actors correctly.
        if (a.sortKey < b.sortKey)
          return true;

        if (a.sortKey > b.sortKey)
          return false;

        // Layer is only a tie-break for items at the same depth:
        // tiles first, shadows next, sprites last.
        return a.renderLayer < b.renderLayer;
      });

  const std::vector<IsometricPointLightSnapshot>* pointLights = nullptr;
  if (registry->hasSystem<IsometricLightingSystem>())
    pointLights =
        &registry->getSystem<IsometricLightingSystem>().getPointLights();

  for (const auto& item : items)
  {

    if (item.isTerrainShadow)
    {
      OpenGLQuadRenderer::SolidQuadDrawCommand command;

      command.points[0] = item.shadowScreenPoints[0];
      command.points[1] = item.shadowScreenPoints[1];
      command.points[2] = item.shadowScreenPoints[2];
      command.points[3] = item.shadowScreenPoints[3];
      command.color = item.tint;

      quadRenderer.submitSolidQuad(command);
      continue;
    }

    // Important: draw queued terrain shadows before the next real tile/sprite.
    // This lets closer tiles/sprites render over already-submitted shadows.
    if (quadRenderer.hasPendingSolidQuads())
    {
      quadRenderer.flushSolidQuads();
      gTerrainShadowFlushes++;
    }

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
      gSpriteProjectedShadowItems++;
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

      if (pointLights)
      {
        const auto& lights = *pointLights;

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

  // Sprites sample the tile they are standing on, not the neighboring raised
  // tile they may visually overlap. This prevents early visual "step up"
  // while approaching an elevated block.
  return glm::floor(transform.position);
}

bool IsometricRenderSystem::tryGetTileElevationAt(const glm::vec2& position,
                                                  int& outElevation) const
{
  const glm::ivec2 tile = gridCellOf(position);

  auto it = tileElevationCache.find(tile);

  if (it == tileElevationCache.end())
  {
    outElevation = 0;
    return false;
  }

  outElevation = it->second;
  return true;
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

void IsometricRenderSystem::markTerrainDirty()
{
  tileElevationCacheDirty = true;

  if (registry && registry->hasSystem<IsometricShadowSystem>())
    registry->getSystem<IsometricShadowSystem>().markTerrainDirty();
}

void IsometricRenderSystem::rebuildTileElevationCache()
{
  tileElevationCache.clear();

  for (const auto& entity : getEntities())
  {
    if (!isTileEntity(entity))
      continue;

    const auto& transform = entity.getComponent<TransformComponent>();

    int elevation = 0;

    if (entity.hasComponent<ElevationComponent>())
      elevation = entity.getComponent<ElevationComponent>().level;

    const glm::ivec2 tile = gridCellOf(transform.position);

    auto it = tileElevationCache.find(tile);

    if (it == tileElevationCache.end())
      tileElevationCache.emplace(tile, elevation);
    else
      it->second = std::max(it->second, elevation);
  }
}

} // namespace sfs
