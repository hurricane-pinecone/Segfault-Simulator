
#include "engine/assetStore/assetStore.h"
#include "engine/components/cameraComponent.h"
#include "engine/components/elevationComponent.h"
#include "engine/components/spriteComponent.h"

#include "engine/components/tags/isometricTile.h"
#include "engine/renderers/commands/commands.h"
#include "engine/renderers/commands/renderCommand.h"
#include "engine/renderers/isometricRenderContext.h"
#include "engine/renderers/quads.h"
#include "engine/renderers/renderContext.h"
#include "engine/systems/isometric/isometricRenderSystem.h"

#include "engine/ecs/registry.h"
#include "engine/logger/logger.h"

#include "engine/renderers/renderPass.h"
#include "engine/systems/cameraSystem.h"
#include "engine/systems/isometric/isometricLightingSystem.h"
#include "engine/systems/isometric/isometricShadowSystem.h"
#include "engine/systems/isometric/isometricSpriteShadowSystem.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include <cstddef>
#include <map>
#include <variant>

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

IsometricRenderSystem::~IsometricRenderSystem() = default;

void IsometricRenderSystem::create()
{
  registerComponent<SpriteComponent>();
  registerComponent<TransformComponent>();

  m_lightingService.setRegistry(registry);
}

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

  m_lightingService.setRegistry(registry);

  beginBatches();
  if (tileElevationCacheDirty)
  {
    rebuildTileElevationCache();
    rebuildTerrainElevationGridView();
    tileElevationCacheDirty = false;
  }

  m_lightingService.updateCacheIfDirty();
  const auto* ambientLighting = m_lightingService.ambient();
  const auto& pointLights = m_lightingService.pointLights();

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
  m_context.terrainElevationGrid = tileElevationGridView;
  m_context.ambientLighting = ambientLighting;
  m_context.pointLights = &pointLights;

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

    LitQuadCommand command;
    command.order = isTileEntity(entity)
                        ? RenderOrder{RenderPass::Terrain, sortKey, 0}
                        : RenderOrder{RenderPass::Objects, sortKey, 0};
    command.textureId = &sprite->textureId;
    command.quad.srcRect = sprite->srcRect;
    command.quad.destRect = dest;
    command.quad.textureWidth = surface->w;
    command.quad.textureHeight = surface->h;

    if (tileEntity)
    {
      command.quad.worldPoints[0] = transform.position;
      command.quad.worldPoints[1] = transform.position + glm::vec2{1.0f, 0.0f};
      command.quad.worldPoints[2] = transform.position + glm::vec2{1.0f, 1.0f};
      command.quad.worldPoints[3] = transform.position + glm::vec2{0.0f, 1.0f};
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

      command.quad.worldPoints[0] = spriteWorldSample;
      command.quad.worldPoints[1] = spriteWorldSample;
      command.quad.worldPoints[2] = spriteWorldSample;
      command.quad.worldPoints[3] = spriteWorldSample;
    }

    if (ambientLighting || !pointLights.empty())
    {
      const auto lighting = m_lightingService.computeLighting({
          spriteWorldSample,
          static_cast<float>(elevationLevel),
      });

      command.quad.lightDirection = lighting.direction;
      command.quad.lightIntensity = lighting.intensity;
      command.quad.ambient = lighting.ambient;
      command.quad.diffuseStrength = lighting.diffuseStrength;
      command.quad.lightColor = lighting.color;
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
          command.quad.hasNormalMap = true;
          command.normalTextureId = &normalSprite->textureId;
        }
      }
    }

    if (!pointLights.empty())
    {
      command.quad.lightCount =
          std::min(static_cast<int>(pointLights.size()), MaxShaderLights);

      for (int i = 0; i < command.quad.lightCount; i++)
      {
        command.quad.lightPositions[i] = pointLights[i].worldPosition;
        command.quad.lightColors[i] = pointLights[i].color;
        command.quad.lightIntensities[i] = pointLights[i].intensity;
        command.quad.lightRadii[i] = pointLights[i].radius;
        command.quad.lightHeights[i] = pointLights[i].height;
      }
    }

    m_renderQueue.submit(command);
  }

  if (const auto shadowSystem = registry->tryGetSystem<IsometricShadowSystem>())
  {
    shadowSystem->computeCommands(m_context);
    m_renderQueue.submitAll(shadowSystem->commands());
  }

  if (const auto spriteShadowSystem =
          registry->tryGetSystem<IsometricSpriteShadowSystem>())
  {
    spriteShadowSystem->computeCommands(m_context);
    m_renderQueue.submitAll(spriteShadowSystem->commands());
  }

  flushBatches();
}

void IsometricRenderSystem::beginBatches() { m_renderQueue.clear(); }

void IsometricRenderSystem::flushBatches()
{
  auto& quadRenderer = RenderContext::quadRenderer();
  auto& commands = m_renderQueue.mutableItems();

  std::vector<AnyRenderCommand> batched;
  batched.reserve(commands.size());

  std::map<std::tuple<RenderOrder, const std::string*, const std::string*>,
           LitQuadBatchCommand>
      litBatches;

  for (const auto& command : commands)
  {
    std::visit(
        [&](const auto& concrete)
        {
          using T = std::decay_t<decltype(concrete)>;

          if constexpr (std::is_same_v<T, LitQuadCommand>)
          {
            if (concrete.order.pass == RenderPass::Terrain)
            {
              auto key = std::make_tuple(
                  concrete.order, concrete.textureId, concrete.normalTextureId);

              auto& batch = litBatches[key];

              batch.order = concrete.order;
              batch.textureId = concrete.textureId;
              batch.normalTextureId = concrete.normalTextureId;
              batch.quad.quads.push_back(concrete.quad);
              return;
            }
          }

          batched.push_back(concrete);
        },
        command);
  }

  for (auto& [_, batch] : litBatches)
    batched.push_back(std::move(batch));

  commands = std::move(batched);

  quadRenderer.begin();

  std::stable_sort(commands.begin(),
                   commands.end(),
                   [](const AnyRenderCommand& a, const AnyRenderCommand& b)
                   {
                     return std::visit(
                         [](const auto& lhs, const auto& rhs)
                         {
                           if (lhs.order.pass != rhs.order.pass)
                             return lhs.order.pass < rhs.order.pass;

                           if (lhs.order.depth != rhs.order.depth)
                             return lhs.order.depth < rhs.order.depth;

                           return lhs.order.subpass < rhs.order.subpass;
                         },
                         a,
                         b);
                   });

  for (const auto& command : commands)
  {
    std::visit(
        [&](const auto& concrete)
        {
          auto drawable = concrete;

          if constexpr (std::is_same_v<std::decay_t<decltype(drawable.quad)>,
                                       LitQuadBatch>)
          {
            for (auto& quad : drawable.quad.quads)
            {
              quad.texture = resolveTexture(drawable.textureId);

              if (quad.texture == 0)
                continue;

              if (drawable.normalTextureId)
              {
                quad.normalTexture = resolveTexture(drawable.normalTextureId);
                quad.hasNormalMap = quad.normalTexture != 0;
              }

              quadRenderer.submit(quad);
            }
          }
          else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(drawable.quad)>,
                                 QuadBatch>)
          {
            for (const auto& quad : drawable.quad.quads)
              quadRenderer.submit(quad);
          }
          else
          {
            if constexpr (requires {
                            drawable.textureId;
                            drawable.quad.texture;
                          })
            {
              drawable.quad.texture = resolveTexture(drawable.textureId);

              if (drawable.quad.texture == 0)
                return;
            }

            if constexpr (requires {
                            drawable.normalTextureId;
                            drawable.quad.normalTexture;
                          })
            {
              drawable.quad.normalTexture =
                  resolveTexture(drawable.normalTextureId);

              drawable.quad.hasNormalMap = drawable.quad.normalTexture != 0;
            }

            quadRenderer.submit(drawable.quad);
          }
        },
        command);
  }

  quadRenderer.flush();
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

unsigned int IsometricRenderSystem::resolveTexture(const std::string* textureId)
{
  if (!textureId)
    return 0;

  SDL_Surface* surface = assetStore.getSurface(*textureId);

  if (!surface)
    return 0;

  return RenderContext::quadRenderer().getOrCreateTexture(*textureId, surface);
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
  return tileElevationGridView.tryGet(gridCellOf(position), outElevation);
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

void IsometricRenderSystem::rebuildTerrainElevationGridView()
{
  tileElevationGridData.clear();
  tileElevationGridView = {};

  if (tileElevationCache.empty())
    return;

  glm::ivec2 min = tileElevationCache.begin()->first;
  glm::ivec2 max = tileElevationCache.begin()->first;

  for (const auto& [tile, elevation] : tileElevationCache)
  {
    min = glm::min(min, tile);
    max = glm::max(max, tile);
  }

  const int width = max.x - min.x + 1;
  const int height = max.y - min.y + 1;

  tileElevationGridData.assign(width * height, -1);

  for (const auto& [tile, elevation] : tileElevationCache)
  {
    const int x = tile.x - min.x;
    const int y = tile.y - min.y;

    tileElevationGridData[y * width + x] = elevation;
  }

  tileElevationGridView.elevations = tileElevationGridData.data();
  tileElevationGridView.width = width;
  tileElevationGridView.height = height;
  tileElevationGridView.stride = width;
  tileElevationGridView.origin = min;
}

IsometricLightingService& IsometricRenderSystem::lighting()
{
  return m_lightingService;
}

const IsometricLightingService& IsometricRenderSystem::lighting() const
{
  return m_lightingService;
}

} // namespace sfs
