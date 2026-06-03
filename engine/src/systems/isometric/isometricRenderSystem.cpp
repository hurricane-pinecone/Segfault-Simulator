
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
#include "engine/utils/profiling.h"

#include "engine/rendering/renderPass.h"
#include "engine/systems/isometric/isometricLightingSystem.h"
#include "engine/systems/isometric/isometricShadowSystem.h"
#include "engine/systems/isometric/isometricSpriteShadowSystem.h"
#include "engine/systems/isometric/isometricWaterSystem.h"
#include "engine/systems/blockGeometrySystem.h"
#include "engine/systems/decalSystem.h"
#include "engine/systems/particleSystem.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <type_traits>
#include <utility>
#include <variant>

namespace sfs
{

namespace
{

// Subpass breaks ties between commands at the same painter depth. Folding it
// into the depth as a tiny epsilon yields one monotonic key per command, which
// maps to clip-space z so the depth buffer orders them the way the painter sort
// intends.
constexpr float kSubpassEpsilon = 1e-4f;

float orderKey(const RenderOrder& order)
{
  return order.depth + static_cast<float>(order.subpass) * kSubpassEpsilon;
}

// Map each command's painter sort-key to a clip-space z (gl_Position.z): onto
// the quad(s) for quad commands, and per-vertex for merged surface meshes.
// Higher sort-key = nearer the camera. Runs before batching so each quad keeps
// its own depth even after tiles are merged into texture batches.
//
// Returns the frame's (minKey, maxKey) so the decal pipeline can normalise its
// GPU-side depth identically (decals own their depth, computed in their shader).
std::pair<float, float> assignClipDepth(std::vector<AnyRenderCommand>& commands)
{
  ZoneScopedN("Render: assignClipDepth");

  float minKey = std::numeric_limits<float>::max();
  float maxKey = std::numeric_limits<float>::lowest();

  const auto includeKey = [&](float key)
  {
    minKey = std::min(minKey, key);
    maxKey = std::max(maxKey, key);
  };

  for (const auto& command : commands)
  {
    std::visit(
        [&](const auto& concrete)
        {
          using T = std::decay_t<decltype(concrete)>;

          // Decals own their depth (computed in the decal shader from the range
          // this function returns), and have no quad to stamp -- skip them.
          if constexpr (std::is_same_v<T, DecalDrawCommand>)
            return;
          else
          {
            // UI/text draws with depth disabled, so it must not skew the range.
            if (concrete.order.pass == RenderPass::UI)
              return;

            if constexpr (std::is_same_v<T, SurfaceCommand> ||
                          std::is_same_v<T, GeometryCommand>)
          {
            // A merged surface / geometry mesh carries a per-vertex world sort-key.
            for (const auto& vertex : concrete.vertices)
              includeKey(vertex.z);
          }
            else if constexpr (requires { concrete.quad.quads; })
            {
              // A merged quad batch (terrain shadows) carries a per-quad world
              // sort-key in z.
              for (const auto& quad : concrete.quad.quads)
                includeKey(quad.z);
            }
            else
            {
              includeKey(orderKey(concrete.order));
            }
          }
        },
        command);
  }

  const float range = maxKey - minKey;
  const float invRange = range > 1e-6f ? 1.0f / range : 0.0f;

  // Higher sort-key (clamped -> 1) maps to the near plane (smaller window
  // depth). NDC headroom of [-0.9, 0.9] leaves room for future always-on-top /
  // always-behind layering without clamping against the clip planes.
  const auto toClipZ = [&](float key)
  {
    const float t = invRange > 0.0f ? (key - minKey) * invRange : 0.5f;
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return 0.9f - 1.8f * clamped;
  };

  for (auto& command : commands)
  {
    std::visit(
        [&](auto& concrete)
        {
          using T = std::decay_t<decltype(concrete)>;

          // Decals carry no quad to remap; their shader derives depth from the
          // returned range.
          if constexpr (std::is_same_v<T, DecalDrawCommand>)
            return;
          else if constexpr (std::is_same_v<T, SurfaceCommand> ||
                             std::is_same_v<T, GeometryCommand>)
          {
            // Remap each vertex's world sort-key to clip-space z in place.
            for (auto& vertex : concrete.vertices)
              vertex.z = toClipZ(vertex.z);
          }
          else if constexpr (requires { concrete.quad.quads; })
          {
            // Merged quad batch (terrain shadows): remap each quad's own world
            // sort-key so a single batch occludes correctly per tile.
            for (auto& quad : concrete.quad.quads)
              quad.z = toClipZ(quad.z);
          }
          else
          {
            concrete.quad.z = toClipZ(orderKey(concrete.order));
          }
        },
        command);
  }

  return {minKey, maxKey};
}

} // namespace

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

void IsometricRenderSystem::setTerrainHeightSource(
    const ITerrainHeightSource* source)
{
  m_terrainHeightSource = source;
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
  ZoneScopedN("IsometricRenderSystem::render");

  // Debug watch
  gTerrainShadowItems = 0;
  gTerrainShadowFlushes = 0;
  gRenderItemCount = 0;
  gTerrainShadowEdgesProcessed = 0;
  gTileRenderItems = 0;
  gSpriteRenderItems = 0;
  gSpriteProjectedShadowItems = 0;
  gTerrainShadowBatchCount = 0;

  // A scene without an injected projection has nothing isometric to draw.
  if (!m_context.projection)
    return;

  const IsometricProjection& proj = *m_context.projection;

  beginBatches();

  if (tileElevationCacheDirty)
  {
    // A terrain height source answers directly from the generator, so the ECS
    // scan (one entity per tile) is only needed as the fallback when no source
    // is wired in.
    if (!m_terrainHeightSource)
      rebuildTileElevationCache();

    // The camera grid position maps to the screen centre, so inverting it gives
    // the tile the heightmap window should be centered on. The window only needs
    // rebuilding when the camera crosses a tile.
    const glm::ivec2 cameraTile =
        gridCellOf(proj.screenToWorld(proj.screenCenter, 0.0f));
    rebuildTerrainElevationGridView(cameraTile);
    tileElevationCacheDirty = false;
  }

  // Re-upload the elevation grid every frame so the shader can occlude point
  // lights against terrain. heightScale converts a light's emitter height into
  // levels.
  //
  // This re-stamps the same window most frames, which looks wasteful, but it is
  // deliberate: a glTexSubImage2D that lands after a gap of frames with no
  // upload produces a one-frame glitch in what the shader samples on this GL
  // driver (the occlusion flashes off for a frame). Uploading only on
  // tile-crossings leaves exactly those gaps and flickers; re-stamping every
  // frame never leaves a gap and is stable. The upload is tiny (49x49 R32F).
  if (tileElevationGridView.valid())
  {
    const float elevationStepWorld =
        static_cast<float>(proj.elevationStep) * proj.worldScale;
    const float heightScale =
        elevationStepWorld > 0.0001f ? 1.0f / elevationStepWorld : 0.0f;

    m_quadRenderer.setHeightmap(tileElevationGridData.data(),
                                tileElevationGridView.width,
                                tileElevationGridView.height,
                                tileElevationGridView.origin.x,
                                tileElevationGridView.origin.y,
                                heightScale);
  }

  // Lights move (the player emits one), so refresh the list every frame. The
  // cache is otherwise only invalidated at setup, which would freeze every light
  // at its first-frame position.
  m_lightingService.invalidateCache();
  m_lightingService.updateCacheIfDirty();
  const auto* ambientLighting = m_lightingService.ambient();
  const auto& pointLights = m_lightingService.pointLights();

  m_context.terrainElevationGrid = tileElevationGridView;
  m_context.ambientLighting = ambientLighting;
  m_context.pointLights = &pointLights;

  // Sun/ambient is identical for every sprite this frame, and the fragment
  // shader applies point lights per pixel, so the directional term is computed
  // once for all sprites. Defaults (no ambient) are a straight-up light at full
  // ambient with no diffuse.
  glm::vec3 sunDirection{0.0f, 0.0f, 1.0f};
  float ambientLevel = 1.0f;
  float sunDiffuseStrength = 0.0f;

  if (ambientLighting)
  {
    sunDirection = ambientLighting->direction;
    ambientLevel = ambientLighting->ambient;
    sunDiffuseStrength = ambientLighting->diffuseStrength;
  }

  // The point-light set is the same for every draw this frame, so bind it once
  // on the renderer.
  PointLightSet pointLightSet;
  pointLightSet.count =
      std::min(static_cast<int>(pointLights.size()), MaxShaderLights);

  // Light radius is authored in screen pixels; the lighting math runs in world
  // tiles, so convert by the on-screen tile width. (Emitter height is converted
  // to elevation levels on the GPU via uHeightScale.) Drop the worldScale factor
  // here -- and the matching one in heightScale -- to make these world-scale
  // independent.
  const float tilePixelWidth =
      static_cast<float>(proj.tileWidth) * proj.worldScale;
  const float radiusPixelsToWorld =
      tilePixelWidth > 0.0001f ? 1.0f / tilePixelWidth : 0.0f;

  for (int i = 0; i < pointLightSet.count; i++)
  {
    pointLightSet.positions[i] = pointLights[i].worldPosition;
    pointLightSet.colors[i] = pointLights[i].color;
    pointLightSet.intensities[i] = pointLights[i].intensity;
    pointLightSet.radii[i] = pointLights[i].radius * radiusPixelsToWorld;
    pointLightSet.heights[i] = pointLights[i].height;

    // The light's ground is the eased elevation of the emitting actor (so a
    // moving light ramps between tile heights), falling back to the raw tile
    // elevation for lights that aren't tracked actors.
    const auto elevationIt = m_actorElevation.find(pointLights[i].entityId);
    pointLightSet.groundLevels[i] =
        elevationIt != m_actorElevation.end()
            ? elevationIt->second
            : static_cast<float>(getTileElevationAt(pointLights[i].worldPosition));
  }

  m_quadRenderer.setPointLights(pointLightSet);

  // Opt-in extension: when a BlockGeometrySystem is present and enabled, terrain
  // tiles render as real face geometry (emitted by that system below) instead of
  // billboard sprites. Non-tile sprites (actors, props) stay billboards either
  // way, so a scene without the system renders exactly as the simple iso path.
  const auto geometrySystem = registry->tryGetSystem<BlockGeometrySystem>();
  const bool geometryTilesActive = geometrySystem && geometrySystem->enabled();

  for (const auto& entity : getEntities())
  {
    if (geometryTilesActive && isTileEntity(entity))
      continue;

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

    const float elevationLevel =
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

    // Terrain tiles and actors are opaque and share the Terrain pass; the depth
    // buffer resolves their occlusion. Each quad's world depth maps to
    // clip-space z in assignClipDepth().
    //
    // Subpass encodes ordering within the same effective depth, folded into
    // clip-z as a tiny epsilon so the tie-break survives:
    //
    //   0 = terrain tiles
    //   1 = actors/sprites
    //   (shadows and water sort later via the Shadow/Surfaces passes)
    //
    // This keeps actors rendering above the tile they stand on while the depth
    // buffer handles block- and sprite-behind-block occlusion.
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

    command.quad.lightDirection = sunDirection;
    command.quad.ambient = ambientLevel;
    command.quad.diffuseStrength = sunDiffuseStrength;

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

    if (tileEntity)
      gTileRenderItems++;
    else
      gSpriteRenderItems++;

    m_renderQueue.submit(command);
  }

  // The sun shadow map (geometry path) replaces the projected terrain shadows,
  // so skip this provider when geometry is active to avoid doubling them up.
  if (const auto shadowSystem = registry->tryGetSystem<IsometricShadowSystem>();
      !geometryTilesActive && shadowSystem && shadowSystem->enabled())
  {
    shadowSystem->computeCommands(m_context);

    const auto& shadowCommands = shadowSystem->commands();
    gTerrainShadowBatchCount = static_cast<int>(shadowCommands.size());
    for (const auto& batch : shadowCommands)
      gTerrainShadowItems += static_cast<int>(batch.quad.quads.size());

    m_renderQueue.submitAll(shadowCommands);
  }

  if (const auto spriteShadowSystem =
          registry->tryGetSystem<IsometricSpriteShadowSystem>();
      spriteShadowSystem && spriteShadowSystem->enabled())
  {
    spriteShadowSystem->computeCommands(m_context);

    gSpriteProjectedShadowItems =
        static_cast<int>(spriteShadowSystem->commands().size());

    m_renderQueue.submitAll(spriteShadowSystem->commands());
  }

  if (const auto waterSystem = registry->tryGetSystem<IsometricWaterSystem>();
      waterSystem && waterSystem->enabled())
  {
    waterSystem->computeCommands(m_context);
    m_renderQueue.submitAll(waterSystem->commands());
  }

  if (const auto particleSystem = registry->tryGetSystem<ParticleSystem>();
      particleSystem && particleSystem->enabled())
  {
    particleSystem->computeCommands(m_context);
    m_renderQueue.submitAll(particleSystem->commands());
  }

  if (const auto decalSystem = registry->tryGetSystem<DecalSystem>();
      decalSystem && decalSystem->enabled())
  {
    decalSystem->computeCommands(m_context);
    m_renderQueue.submitAll(decalSystem->commands());
  }

  if (geometryTilesActive)
  {
    geometrySystem->computeCommands(m_context);
    m_renderQueue.submitAll(geometrySystem->commands());
  }

  flushBatches();
}

void IsometricRenderSystem::beginBatches() { m_renderQueue.clear(); }

void IsometricRenderSystem::flushBatches()
{
  ZoneScopedN("Render: flushBatches");

  auto& quadRenderer = m_quadRenderer;
  auto& commands = m_renderQueue.mutableItems();

  gRenderItemCount = static_cast<int>(commands.size());

  // Stamp each quad's clip-space depth before batching, so tiles merged by
  // texture occlude correctly via the depth buffer.
  const auto [depthMin, depthMax] = assignClipDepth(commands);

  batchTerrainTiles(commands);
  sortRenderCommands(commands);

  quadRenderer.begin();

  quadRenderer.setSurfaceTime(static_cast<float>(SDL_GetTicks()) / 1000.0f);

  // Hand the decal pipeline the projection + the frame's depth range so its
  // shader projects world-space decals and normalises clip-z exactly like the
  // rest of the scene (so decals occlude against terrain correctly).
  if (m_context.projection)
  {
    const IsometricProjection& proj = *m_context.projection;
    const float range = depthMax - depthMin;

    DecalFrameParams dp;
    dp.tileWidth = static_cast<float>(proj.tileWidth);
    dp.tileHeight = static_cast<float>(proj.tileHeight);
    dp.worldScale = proj.worldScale;
    dp.zoom = proj.zoom;
    dp.cameraIso = proj.cameraIso;
    dp.screenCenter = proj.screenCenter;
    dp.elevationStep = static_cast<float>(proj.elevationStep);
    dp.depthMin = depthMin;
    dp.depthInvRange = range > 1e-6f ? 1.0f / range : 0.0f;

    if (m_context.ambientLighting)
    {
      dp.ambient = m_context.ambientLighting->ambient;
      dp.ambientColor = m_context.ambientLighting->color;
    }

    quadRenderer.setDecalFrameParams(dp);
  }

  // Geometry faces share the scene's sun + ambient lighting (point lights and
  // the heightmap are already bound via setPointLights / setHeightmap).
  if (m_context.ambientLighting)
  {
    const auto& al = *m_context.ambientLighting;
    quadRenderer.setGeometryLighting(
        al.ambient, al.color, al.direction, al.diffuseStrength);
  }

  {
    ZoneScopedN("Render: submit loop");

    for (const auto& command : commands)
      submitRenderCommand(command, quadRenderer);
  }

  {
    ZoneScopedN("Render: final flush");
    quadRenderer.flush();
  }
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
  ZoneScopedN("Render: resolveTexture");

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

float IsometricRenderSystem::getRenderElevationLevel(
    const Entity& entity,
    const glm::vec2& samplePosition) const
{
  if (isTileEntity(entity))
  {
    if (entity.hasComponent<ElevationComponent>())
      return static_cast<float>(entity.getComponent<ElevationComponent>().level);

    return 0.0f;
  }

  // Actors ride the eased elevation so a step up/down ramps instead of snapping.
  return smoothedElevationOf(entity, samplePosition);
}

float IsometricRenderSystem::smoothedElevationOf(
    const Entity& entity,
    const glm::vec2& samplePosition) const
{
  const auto it = m_actorElevation.find(entity.getId());

  if (it != m_actorElevation.end())
    return it->second;

  return static_cast<float>(getTileElevationAt(samplePosition));
}

void IsometricRenderSystem::update(double deltaTime)
{
  updateActorElevations(deltaTime);
}

void IsometricRenderSystem::updateActorElevations(double deltaTime)
{
  // Without a populated elevation window every tile reads as 0; wait for it so
  // actors initialise (snap) to their true height instead of easing up from 0.
  if (!tileElevationGridView.valid())
    return;

  // Frame-rate independent exponential ease. ~0.08s to close most of the gap, so
  // a step up/down reads as a quick smooth move rather than a teleport.
  constexpr float kElevationEaseRate = 14.0f;
  const float factor = static_cast<float>(1.0 - std::exp(-deltaTime * kElevationEaseRate));

  for (const auto& entity : getEntities())
  {
    if (isTileEntity(entity))
      continue;

    const auto& transform = entity.getComponent<TransformComponent>();

    // Target is the discrete tile the actor stands on (floor()), matching the
    // gameplay elevation -- the easing only smooths how the render/light catch up
    // to it, so the actor never floats up a cliff before it's actually on top.
    const float target =
        static_cast<float>(getTileElevationAt(glm::floor(transform.position)));

    const auto it = m_actorElevation.find(entity.getId());

    if (it == m_actorElevation.end())
      m_actorElevation.emplace(entity.getId(), target); // snap on first sight
    else
      it->second += (target - it->second) * factor;
  }
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

void IsometricRenderSystem::rebuildTerrainElevationGridView(
    const glm::ivec2& centerTile)
{
  tileElevationGridData.clear();
  tileElevationGridView = {};

  if (!m_terrainHeightSource && tileElevationCache.empty())
    return;

  // A fixed-size window centered on the camera. A height source answers every
  // cell directly, so the window is always complete. The ECS fallback instead
  // reads tiles that briefly linger after being unloaded or are missing while
  // streaming in; a window a tile inside the loaded area on each side keeps the
  // bounds stable, but its leading edge can still hole for a frame -- which is
  // exactly the flicker the height source removes.
  constexpr int kHalfSpan = 24; // the terrain loads 25 tiles in each direction
  const glm::ivec2 min = centerTile - glm::ivec2(kHalfSpan);
  const int span = kHalfSpan * 2 + 1;

  tileElevationGridData.assign(span * span, EmptyElevation);

  for (int y = 0; y < span; y++)
  {
    for (int x = 0; x < span; x++)
    {
      const glm::ivec2 tile = min + glm::ivec2(x, y);

      if (m_terrainHeightSource)
      {
        tileElevationGridData[y * span + x] =
            m_terrainHeightSource->terrainHeightAt(tile.x, tile.y);
        continue;
      }

      const auto it = tileElevationCache.find(tile);

      if (it != tileElevationCache.end())
        tileElevationGridData[y * span + x] = it->second;
    }
  }

  tileElevationGridView.elevations = tileElevationGridData.data();
  tileElevationGridView.width = span;
  tileElevationGridView.height = span;
  tileElevationGridView.stride = span;
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
  ZoneScopedN("Render: sortRenderCommands");

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
  ZoneScopedN("Render: batchTerrainTiles");

  std::vector<AnyRenderCommand> batched;
  batched.reserve(commands.size());

  // Group by material (texture + normal map + surface effect), not by painter
  // depth: the depth buffer resolves occlusion, so all tiles and sprites
  // sharing a material merge into one draw call regardless of depth, keeping
  // the batch-flush count down to one per material per frame.
  std::map<std::tuple<const std::string*,
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
              auto key = std::make_tuple(concrete.textureId,
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
