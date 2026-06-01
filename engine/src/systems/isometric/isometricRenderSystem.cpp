
#include "engine/assetStore/assetStore.h"
#include "engine/components/elevationComponent.h"
#include "engine/components/spriteComponent.h"

#include "engine/components/surfaceEffect.h"
#include "engine/components/tags/isometricTile.h"
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/commands/renderCommand.h"
#include "engine/rendering/isometricRenderContext.h"
#include "engine/rendering/quads.h"
#include "engine/systems/isometric/isometricRenderSystem.h"

#include "engine/ecs/registry.h"
#include "engine/logger/logger.h"

#include "engine/rendering/renderPass.h"
#include "engine/systems/isometric/isometricLightingSystem.h"
#include "engine/systems/isometric/isometricShadowSystem.h"
#include "engine/systems/isometric/isometricSpriteShadowSystem.h"
#include "engine/systems/isometric/isometricWaterSystem.h"
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
int gTerrainShadowBatchCount = 0;

IsometricRenderSystem::IsometricRenderSystem(AssetStore& assetStore,
                                             IQuadRenderer& quadRenderer)
    : assetStore(assetStore), m_quadRenderer(quadRenderer)
{
}

void IsometricRenderSystem::setProjection(const IsometricProjection* projection)
{
  m_context.projection = projection;
}

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

  // A scene without an injected projection has nothing isometric to draw.
  if (!m_context.projection)
    return;

  const IsometricProjection& proj = *m_context.projection;

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
    const glm::vec2 isoPosition = gridToIsometric(
        transform.position, proj.tileWidth, proj.tileHeight, proj.worldScale);

    const float zoom = proj.zoom;

    const glm::vec2 screenPosition =
        (isoPosition - proj.cameraIso) * zoom + proj.screenCenter;

    const int width = static_cast<int>(sprite->srcRect.w * transform.scale.x *
                                       proj.worldScale * zoom);

    const int height = static_cast<int>(sprite->srcRect.h * transform.scale.y *
                                        proj.worldScale * zoom);

    // Anchors define how the sprite image attaches to its world position.
    // Blocks can use a top anchor while actors use a feet/bottom anchor.
    // Sorting below does not assume every sprite has the same anchor.
    const float anchorX = spriteComponent.anchor.x * static_cast<float>(width);
    const float anchorY = spriteComponent.anchor.y * static_cast<float>(height);

    const glm::vec2 groundSamplePosition =
        getGroundSamplePosition(entity, transform);

    const int elevationLevel =
        getRenderElevationLevel(entity, groundSamplePosition);

    const int elevationOffset = static_cast<int>(std::round(
        elevationLevel * proj.elevationStep * proj.worldScale * zoom));

    const glm::vec2 surfacePosition{
        screenPosition.x,
        screenPosition.y - elevationOffset,
    };

    const glm::vec2 visualPosition =
        surfacePosition + spriteComponent.renderOffset * zoom;

    SDL_Rect dest{
        static_cast<int>(std::round(visualPosition.x - anchorX)),
        static_cast<int>(std::round(visualPosition.y - anchorY)),
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

    // Terrain tiles, actors, water, and shadows all share the same Terrain
    // painter pass so they can interleave correctly by world depth.
    //
    // Subpasses only resolve ordering *within the same effective depth*:
    //
    //   0 = terrain tiles
    //   1 = actors/sprites
    //   2 = water surfaces
    //
    // This allows:
    // - actors to render above the terrain tile they stand on
    // - water to render over actors standing in shallow water
    // - terrain elevation painter sorting to remain stable
    //
    // Using separate render passes for terrain/water/objects caused global
    // ordering artifacts because entire categories rendered before/after others
    // instead of participating in the same world-depth sort.
    LitQuadCommand command;
    command.order = isTileEntity(entity)
                        ? RenderOrder{RenderPass::Terrain, sortKey, 0}
                        : RenderOrder{RenderPass::Terrain, sortKey, 1};
    command.textureId = &sprite->textureId;
    command.quad.srcRect = sprite->srcRect;
    command.quad.destRect = dest;
    command.quad.textureWidth = surface->w;
    command.quad.textureHeight = surface->h;

    if (entity.hasComponent<SurfaceEffect>())
      command.type = entity.getComponent<SurfaceEffect>().type;

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

  if (const auto waterSystem = registry->tryGetSystem<IsometricWaterSystem>())
  {
    waterSystem->computeCommands(m_context);
    m_renderQueue.submitAll(waterSystem->commands());
  }

  flushBatches();
}

void IsometricRenderSystem::beginBatches() { m_renderQueue.clear(); }

void IsometricRenderSystem::flushBatches()
{
  auto& quadRenderer = m_quadRenderer;
  auto& commands = m_renderQueue.mutableItems();

  batchTerrainTiles(commands);
  sortRenderCommands(commands);

  quadRenderer.begin();

  quadRenderer.setSurfaceTime(static_cast<float>(SDL_GetTicks()) / 1000.0f);

  for (const auto& command : commands)
    submitRenderCommand(command, quadRenderer);

  quadRenderer.flush();
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
  auto& quadRenderer = m_quadRenderer;

  const float e = static_cast<float>(elevation);

  const glm::vec2 points[5] = {
      m_context.worldToScreen({gridPosition.x, gridPosition.y}, e),
      m_context.worldToScreen({gridPosition.x + 1.0f, gridPosition.y}, e),
      m_context.worldToScreen(
          {gridPosition.x + 1.0f, gridPosition.y + 1.0f}, e),
      m_context.worldToScreen({gridPosition.x, gridPosition.y + 1.0f}, e),
      m_context.worldToScreen({gridPosition.x, gridPosition.y}, e),
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

  return m_quadRenderer.getOrCreateTexture(*textureId, surface);
}

glm::vec2 IsometricRenderSystem::screenToWorld(const glm::vec2& screenPosition,
                                               float elevation) const
{
  if (!m_context.projection)
    return {};

  return m_context.projection->screenToWorld(screenPosition, elevation);
}

TilePick IsometricRenderSystem::pickTile(const glm::vec2& screenPosition) const
{
  if (!m_context.projection)
    return {};

  return sfs::pickTile(
      screenPosition, *m_context.projection, tileElevationGridView);
}

glm::ivec2
IsometricRenderSystem::screenToTile(const glm::vec2& screenPosition) const
{
  return pickTile(screenPosition).tile;
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

void IsometricRenderSystem::markTerrainDirty()
{
  tileElevationCacheDirty = true;

  if (registry && registry->hasSystem<IsometricShadowSystem>())
    registry->getSystem<IsometricShadowSystem>().markTerrainDirty();
}

void IsometricRenderSystem::rebuildTileElevationCache()
{
  tileElevationCache.clear();

  auto tiles =
      registry->view<TransformComponent, IsometricTile, ElevationComponent>();

  for (const auto& entity : tiles)
  {
    const auto& transform = entity.getComponent<TransformComponent>();
    const int elevation = entity.getComponent<ElevationComponent>().level;

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

  tileElevationGridData.assign(width * height, EmptyElevation);

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

void IsometricRenderSystem::sortRenderCommands(
    std::vector<AnyRenderCommand>& commands)
{
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
}

void IsometricRenderSystem::batchTerrainTiles(
    std::vector<AnyRenderCommand>& commands)
{
  std::vector<AnyRenderCommand> batched;
  batched.reserve(commands.size());

  std::map<std::tuple<RenderOrder,
                      const std::string*,
                      const std::string*,
                      SurfaceEffect::Type>,
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
              auto key = std::make_tuple(concrete.order,
                                         concrete.textureId,
                                         concrete.normalTextureId,
                                         concrete.type);

              auto& batch = litBatches[key];

              batch.order = concrete.order;
              batch.textureId = concrete.textureId;
              batch.normalTextureId = concrete.normalTextureId;
              batch.type = concrete.type;
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
}

void IsometricRenderSystem::submitRenderCommand(
    const AnyRenderCommand& command,
    IQuadRenderer& quadRenderer)
{
  std::visit([&](const auto& concrete)
             { submitConcreteRenderCommand(concrete, quadRenderer); },
             command);
}

} // namespace sfs
