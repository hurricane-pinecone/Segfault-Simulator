
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/core/components/elevationComponent.h"
#include "engine/core/components/boxCollider2D.h"
#include "engine/core/components/spriteComponent.h"
#include "engine/core/components/worldCollider.h"

#include "engine/core/components/surfaceEffect.h"
#include "engine/core/components/tags/isometricTile.h"
#include "engine/runtime/rendering/commands/commands.h"
#include "engine/runtime/rendering/commands/renderCommand.h"
#include "engine/runtime/rendering/isometricRenderContext.h"
#include "engine/runtime/rendering/quads.h"
#include "engine/runtime/rendering/renderStats.h"
#include "engine/runtime/systems/isometric/isometricRenderSystem.h"

#include "engine/core/ecs/registry.h"
#include "engine/core/logger/logger.h"
#include "engine/core/util/profiling.h"

#include "engine/core/components/lightEmitterComponent.h"
#include "engine/runtime/rendering/modules/blockGeometry.h"
#include "engine/runtime/rendering/modules/decals.h"
#include "engine/runtime/rendering/modules/terrainShadow.h"
#include "engine/runtime/rendering/renderPass.h"
#include "glm/glm/common.hpp"
#include "glm/glm/exponential.hpp"
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
// GPU-side depth identically (decals own their depth, computed in their
// shader).
std::pair<float, float> assignClipDepth(std::vector<AnyRenderCommand>& commands)
{
  ZoneScopedN("Render: assignClipDepth");

  float minKey = std::numeric_limits<float>::max();
  float maxKey = std::numeric_limits<float>::lowest();

  const auto includeKey = [&](float key)
  {
    minKey = glm::min(minKey, key);
    maxKey = glm::max(maxKey, key);
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
              // A merged surface / geometry mesh carries a per-vertex world
              // sort-key.
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
              const float feetKey = orderKey(concrete.order);
              includeKey(feetKey);

              // A sprite's top edge sits nearer than its feet (vertical
              // billboard), so its key reaches further toward the camera.
              if constexpr (std::is_same_v<T, LitQuadCommand>)
                includeKey(feetKey + concrete.quad.depthSpan);
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
    const float clamped = glm::clamp(t, 0.0f, 1.0f);
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
            const float feetKey = orderKey(concrete.order);
            concrete.quad.z = toClipZ(feetKey);

            // Give sprites a depth gradient over their height that matches the
            // block-face geometry's elevation weighting, so a billboard sorts
            // per-pixel like the vertical surface it depicts.
            if constexpr (std::is_same_v<T, LitQuadCommand>)
              concrete.quad.zTop = toClipZ(feetKey + concrete.quad.depthSpan);
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
    : assetStore(assetStore),
      m_quadRenderer(dynamic_cast<IIsometricRenderer&>(quadRenderer))
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

IDecalSink* IsometricRenderSystem::decalSink()
{
  Decals* decals = module<Decals>();
  if (!decals)
    decals = &withModule<Decals>();
  return decals;
}

void IsometricRenderSystem::create()
{
  registerComponent<SpriteComponent>();
  registerComponent<TransformComponent>();
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
    // the tile the heightmap window should be centered on. The window only
    // needs rebuilding when the camera crosses a tile.
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

  // Lights move (the player emits one), so refresh the list every frame.
  gatherPointLights();
  const auto* ambientLighting = m_hasAmbient ? &m_ambient : nullptr;
  const auto& pointLights = m_pointLights;

  m_context.terrainElevationGrid = tileElevationGridView;
  m_context.ambientLighting = ambientLighting;
  m_context.pointLights = &m_pointLights;

  // Sun/ambient is identical for every sprite this frame, and the fragment
  // shader applies point lights per pixel, so the directional term is computed
  // once for all sprites. Defaults (no ambient) are a straight-up light at full
  // ambient with no diffuse.
  glm::vec3 sunDirection{0.0f, 0.0f, 1.0f};
  glm::vec3 sunColor{1.0f, 1.0f, 1.0f};
  float ambientLevel = 1.0f;
  float sunDiffuseStrength = 0.0f;

  if (ambientLighting)
  {
    sunDirection = ambientLighting->direction;
    sunColor = ambientLighting->color;
    ambientLevel = ambientLighting->ambient;
    sunDiffuseStrength = ambientLighting->diffuseStrength;
  }

  // The point-light set is the same for every draw this frame, so bind it once
  // on the renderer.
  PointLightSet pointLightSet;
  pointLightSet.count =
      glm::min(static_cast<int>(pointLights.size()), MaxShaderLights);

  // Light radius is authored in screen pixels; the lighting math runs in world
  // tiles, so convert by the on-screen tile width. (Emitter height is converted
  // to elevation levels on the GPU via uHeightScale.) Drop the worldScale
  // factor here -- and the matching one in heightScale -- to make these
  // world-scale independent.
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
    pointLightSet.groundLevels[i] = elevationIt != m_actorElevation.end()
                                        ? elevationIt->second
                                        : static_cast<float>(getTileElevationAt(
                                              pointLights[i].worldPosition));
  }

  m_quadRenderer.setPointLights(pointLightSet);

  // When the BlockGeometry module is registered, terrain tiles render as real
  // face geometry (emitted by that module below) instead of billboard sprites.
  // Non-tile sprites (actors, props) stay billboards either way, so a scene
  // without the module renders exactly as the simple iso path. Cross-cutting
  // state is derived from module presence and published into the context.
  const bool geometryActive = hasModule<BlockGeometry>();
  m_context.geometryActive = geometryActive;

  // The block-geometry render style takes the in-shader heightmap march; the
  // billboard style takes projected shadow quads (the TerrainShadow module).
  // The march runs only when both geometry and terrain shadows are present.
  m_quadRenderer.setSunShadowMarchEnabled(geometryActive &&
                                          hasModule<TerrainShadow>());

  for (const auto& entity : getEntities())
  {
    if (geometryActive && isTileEntity(entity))
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

    // The sprite's visual rise AND its depth both use the discrete standing
    // elevation, so stepping a level snaps the sprite straight up/down instead
    // of easing along a diagonal (the eased path slid the sprite through the
    // block edge mid-step). The EASED elevation is kept only for point-light
    // occlusion (m_actorElevation, applied in setPointLights), so lighting
    // still ramps smoothly across a step. Actors derive it from their foot
    // collider (if any); tiles use their own elevation.
    const float elevationLevel =
        isTileEntity(entity) ? tileElevationLevel(entity)
                             : actorStandingElevation(entity, transform);

    const int elevationOffset = static_cast<int>(glm::round(
        elevationLevel * proj.elevationStep * proj.worldScale * zoom));

    const glm::vec2 surfacePosition{
        screenPosition.x,
        screenPosition.y - elevationOffset,
    };

    const glm::vec2 visualPosition =
        surfacePosition + spriteComponent.renderOffset * zoom;

    SDL_Rect dest{
        static_cast<int>(glm::round(visualPosition.x - anchorX)),
        static_cast<int>(glm::round(visualPosition.y - anchorY)),
        width,
        height,
    };

    const bool tileEntity = isTileEntity(entity);

    // Sort by the exact ground-contact position so the sprite's single quad
    // depth matches where it is drawn. Quantising actors to the cell centre put
    // their depth at the middle of the tile's depth range, so the near half of
    // the tile's own top face read as nearer and the depth buffer clipped the
    // actor through it (correct painter order pre-depth-buffer, wrong now).
    const glm::vec2 sortPosition = transform.position;

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

    float sortKey =
        sortPosition.x + sortPosition.y + elevationLevel * ElevationSortWeight;

    // Sprites sort just in front of the tile they stand on so they aren't
    // z-fought by their own ground (the subpass also separates them from
    // tiles).
    if (!tileEntity)
      sortKey += SpriteBias;

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
      command.quad.worldPoints[0] = spriteWorldSample;
      command.quad.worldPoints[1] = spriteWorldSample;
      command.quad.worldPoints[2] = spriteWorldSample;
      command.quad.worldPoints[3] = spriteWorldSample;

      // Block geometry is real 3D faces whose depth rises with elevation
      // (z = x+y + ground*ElevationSortWeight). A flat billboard would sort by
      // a single depth and clip against those faces, so give the actor a
      // matching depth gradient over its height -- feet at its standing depth,
      // head one sprite-height of elevation nearer -- and let the depth buffer
      // occlude it per-pixel like the vertical surface it depicts. Billboard
      // terrain is flat, so there the actor stays flat (depthSpan 0) and sorts
      // against the flat tiles exactly as before.
      if (geometryActive)
      {
        const float spriteLevels = static_cast<float>(sprite->srcRect.h) *
                                   transform.scale.y /
                                   static_cast<float>(proj.elevationStep);
        command.quad.depthSpan = spriteLevels * ElevationSortWeight;
      }
    }

    command.quad.lightDirection = sunDirection;
    command.quad.lightColor = sunColor;
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

  // Each registered module appends its frame commands; the queue's sort orders
  // them against the tiles/sprites above by (pass, depth, subpass), so emit
  // order does not matter. A feature draws iff its module is present.
  std::vector<AnyRenderCommand> scratch;
  for (auto& [type, module] : m_modules)
  {
    scratch.clear();
    module->emit(m_context, scratch);
    m_renderQueue.submitAll(scratch);
  }

  // Surface the terrain-shadow debug counters from the module when present.
  if (const auto* terrainShadow = module<TerrainShadow>())
  {
    const auto& shadowCommands = terrainShadow->commands();
    gTerrainShadowBatchCount = static_cast<int>(shadowCommands.size());
    for (const auto& batch : shadowCommands)
      gTerrainShadowItems += static_cast<int>(batch.quad.quads.size());
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

void IsometricRenderSystem::drawDebugColliders(SDL_Color worldColor,
                                               SDL_Color screenColor)
{
  if (!m_context.projection)
    return;

  const auto& proj = *m_context.projection;

  // WorldColliders are a ground-plane AABB (world tiles), so project their
  // bounds isometrically: the outline is a diamond lying on the terrain at the
  // entity's elevation.
  for (const auto& entity : registry->view<WorldCollider, TransformComponent>())
  {
    const auto& collider = entity.getComponent<WorldCollider>();
    const auto& transform = entity.getComponent<TransformComponent>();

    const float e = isTileEntity(entity)
                        ? tileElevationLevel(entity)
                        : actorStandingElevation(entity, transform);

    const glm::vec2 points[5] = {
        m_context.worldToScreen({collider.left(), collider.top()}, e),
        m_context.worldToScreen({collider.right(), collider.top()}, e),
        m_context.worldToScreen({collider.right(), collider.bottom()}, e),
        m_context.worldToScreen({collider.left(), collider.bottom()}, e),
        m_context.worldToScreen({collider.left(), collider.top()}, e),
    };

    m_quadRenderer.drawLineLoop(points, 5, worldColor);
  }

  // BoxCollider2D is a screen-space hit box centred on the entity (position +
  // offset), in sprite pixels scaled like the sprite -- so it overlays the art
  // directly (sprite Y and screen Y both grow downward, elevation already baked
  // into the screen position). Draw a centred box of half-extents * pxScale.
  for (const auto& entity :
       registry->view<BoxCollider2D, TransformComponent, SpriteComponent>())
  {
    const auto& collider = entity.getComponent<BoxCollider2D>();
    const auto& transform = entity.getComponent<TransformComponent>();

    const float elevation = isTileEntity(entity)
                                ? tileElevationLevel(entity)
                                : actorStandingElevation(entity, transform);

    const glm::vec2 pxScale{transform.scale.x * proj.worldScale * proj.zoom,
                            transform.scale.y * proj.worldScale * proj.zoom};

    const glm::vec2 center =
        m_context.worldToScreen(transform.position, elevation) +
        collider.offset * pxScale;
    const glm::vec2 boxHalf = collider.half * pxScale;
    const glm::vec2 topLeft = center - boxHalf;
    const glm::vec2 boxSize = boxHalf * 2.0f;

    const glm::vec2 points[5] = {
        topLeft,
        {topLeft.x + boxSize.x, topLeft.y},
        {topLeft.x + boxSize.x, topLeft.y + boxSize.y},
        {topLeft.x, topLeft.y + boxSize.y},
        topLeft,
    };

    m_quadRenderer.drawLineLoop(points, 5, screenColor);
  }
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

float IsometricRenderSystem::tileElevationLevel(const Entity& entity) const
{
  if (entity.hasComponent<ElevationComponent>())
    return static_cast<float>(entity.getComponent<ElevationComponent>().level);

  return 0.0f;
}

float IsometricRenderSystem::actorStandingElevation(
    const Entity& entity,
    const TransformComponent& transform) const
{
  // No world collider: stand on the tile under the actor's position.
  if (!entity.hasComponent<WorldCollider>())
    return static_cast<float>(
        getTileElevationAt(glm::floor(transform.position)));

  // Drive elevation from the footprint: stand on the highest CLIMBABLE (<= +1)
  // tile it covers, so the actor steps up the instant its feet reach a raised
  // tile. A narrow footprint means the body/arms don't trigger it.
  const auto& foot = entity.getComponent<WorldCollider>();
  const glm::vec2 lo = transform.position + foot.worldOffset();
  const glm::vec2 hi = lo + foot.worldSize();
  const glm::vec2 center = (lo + hi) * 0.5f;

  const int centerLevel = getTileElevationAt(glm::floor(center));
  int level = centerLevel;

  const glm::vec2 corners[4] = {lo, {hi.x, lo.y}, {lo.x, hi.y}, hi};
  for (const glm::vec2& corner : corners)
  {
    const int e = getTileElevationAt(glm::floor(corner));
    if (e > level && e <= centerLevel + 1)
      level = e;
  }

  return static_cast<float>(level);
}

void IsometricRenderSystem::update(double deltaTime)
{
  updateActorElevations(deltaTime);

  updateModules(deltaTime);
}

void IsometricRenderSystem::updateActorElevations(double deltaTime)
{
  // Without a populated elevation window every tile reads as 0; wait for it so
  // actors initialise (snap) to their true height instead of easing up from 0.
  if (!tileElevationGridView.valid())
    return;

  // Frame-rate independent exponential ease. ~0.08s to close most of the gap,
  // so a step up/down reads as a quick smooth move rather than a teleport.
  constexpr float kElevationEaseRate = 14.0f;
  const float factor =
      static_cast<float>(1.0 - glm::exp(-deltaTime * kElevationEaseRate));

  for (const auto& entity : getEntities())
  {
    if (isTileEntity(entity))
      continue;

    const auto& transform = entity.getComponent<TransformComponent>();

    // Target is the discrete tile the actor stands on (floor()), matching the
    // gameplay elevation -- the easing only smooths how the render/light catch
    // up to it, so the actor never floats up a cliff before it's actually on
    // top.
    const float target =
        static_cast<float>(getTileElevationAt(glm::floor(transform.position)));

    const auto it = m_actorElevation.find(entity.getId());

    if (it == m_actorElevation.end())
      m_actorElevation.emplace(entity.getId(), target); // snap on first sight
    else
      it->second += (target - it->second) * factor;
  }
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

  if (auto* terrainShadow = module<TerrainShadow>())
    terrainShadow->markTerrainDirty();
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
      it->second = glm::max(it->second, elevation);
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

void IsometricRenderSystem::setAmbientLighting(
    const IsometricAmbientLighting& ambient)
{
  m_ambient = ambient;
  m_hasAmbient = true;
}

void IsometricRenderSystem::gatherPointLights()
{
  m_pointLights.clear();

  if (!registry)
    return;

  for (const auto& entity :
       registry->view<TransformComponent, LightEmitterComponent>())
  {
    const auto& transform = entity.getComponent<TransformComponent>();
    const auto& light = entity.getComponent<LightEmitterComponent>();

    m_pointLights.push_back({
        transform.position,
        light.height,
        light.color,
        light.intensity,
        light.radius,
        entity.getId(),
    });
  }
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
  std::map<
      std::tuple<const std::string*, const std::string*, SurfaceEffect::Type>,
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
                  concrete.textureId, concrete.normalTextureId, concrete.type);

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
    IIsometricRenderer& quadRenderer)
{
  std::visit([&](const auto& concrete)
             { submitConcreteRenderCommand(concrete, quadRenderer); },
             command);
}

void IsometricRenderSystem::setSunShadowStyle(SunShadowStyle style)
{
  m_sunShadowStyle = style;
  m_quadRenderer.setSunShadowStyle(style);
}

void IsometricRenderSystem::forEachModule(
    const std::function<void(std::type_index,
                             IRenderModule<IsometricRenderContext>&,
                             const IsometricRenderContext&)>& fn)
{
  // Refresh the cross-cutting flags a module reads to pick mode-appropriate
  // settings (the same derivation render() performs each frame).
  m_context.geometryActive = hasModule<BlockGeometry>();

  for (auto& [type, module] : m_modules)
    fn(type, *module, m_context);
}

} // namespace sfs
