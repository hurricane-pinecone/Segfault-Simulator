#include "engine/systems/isometricRenderSystem.h"

#include "engine/components/cameraComponent.h"
#include "engine/components/elevationComponent.h"
#include "engine/components/spriteComponent.h"
#include "engine/components/tags/isometricTile.h"
#include "engine/ecs/registry.h"
#include "engine/logger/logger.h"
#include "engine/renderers/renderContext.h"
#include "engine/systems/cameraSystem.h"
#include "engine/systems/isometricLightingSystem.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/geometric.hpp"

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
  if (tileElevationCache.empty())
    rebuildTileElevationCache();

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

    RenderItem item;

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

    std::unordered_set<std::string> visitedGroundTiles;
    std::unordered_set<std::string> visitedWallFaces;

    if (tileEntity)
    {
      item.worldPoints[0] = transform.position;
      item.worldPoints[1] = transform.position + glm::vec2{1.0f, 0.0f};
      item.worldPoints[2] = transform.position + glm::vec2{1.0f, 1.0f};
      item.worldPoints[3] = transform.position + glm::vec2{0.0f, 1.0f};

      // const glm::ivec2 tile = gridCellOf(transform.position);

      // if (tile.x == 15 && tile.y == 17)
      // {
      //   submitWallShadowFace(tile, elevationLevel, 2, item);
      //   submitWallShadowFace(tile, elevationLevel, 3, item);
      // }

      // DEBUG
      // const glm::ivec2 tile = gridCellOf(transform.position);
      // const int elevation = elevationLevel;
      //
      // if (elevation > 0)
      // {
      //   const int leftElevation =
      //       getTileElevationAt(glm::vec2{tile.x - 1, tile.y});
      //
      //   const int backElevation =
      //       getTileElevationAt(glm::vec2{tile.x, tile.y - 1});
      //
      //   const int rightElevation =
      //       getTileElevationAt(glm::vec2{tile.x + 1, tile.y});
      //
      //   const int frontElevation =
      //       getTileElevationAt(glm::vec2{tile.x, tile.y + 1});
      //
      //   if (leftElevation < elevation)
      //     drawDebugWallFace(tile, elevation, 0, SDL_Color{255, 0, 255, 255});
      //
      //   if (backElevation < elevation)
      //     drawDebugWallFace(tile, elevation, 1, SDL_Color{255, 0, 255, 255});
      //
      //   if (rightElevation < elevation)
      //     drawDebugWallFace(tile, elevation, 2, SDL_Color{255, 0, 255, 255});
      //
      //   if (frontElevation < elevation)
      //     drawDebugWallFace(tile, elevation, 3, SDL_Color{255, 0, 255, 255});
      // }
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

    if (lightingSystem && !tileEntity)
    {
      const glm::vec3 lightDir3D = lightingSystem->getLightDirection();

      glm::vec2 lightDir2D{
          lightDir3D.x,
          lightDir3D.y,
      };

      if (glm::length(lightDir2D) <= 0.0001f)
      {
        submitSprite(item);
        continue;
      }

      lightDir2D = glm::normalize(lightDir2D);

      // Shadows cast away from the sun.
      const glm::vec2 shadowDir = -lightDir2D;
      const glm::vec2 shadowSide{-shadowDir.y, shadowDir.x};

      const float spriteWorldHeight =
          static_cast<float>(item.dest.h) /
          (static_cast<float>(tileHeight) * worldScale * zoom);

      const float shadowLength =
          ShadowLength * std::clamp(spriteWorldHeight * 0.35f, 0.75f, 2.5f);

      const float spriteWorldWidth =
          static_cast<float>(item.dest.w) /
          (static_cast<float>(tileWidth) * worldScale * zoom);

      const float casterFootWidth =
          std::clamp(spriteWorldWidth * 0.22f, 0.15f, 0.75f);

      const float casterFootDepth =
          std::clamp(spriteWorldWidth * 0.14f, 0.08f, 0.45f);

      const glm::vec2 casterA = spriteWorldSample -
                                shadowSide * casterFootWidth -
                                shadowDir * casterFootDepth;

      const glm::vec2 casterB = spriteWorldSample +
                                shadowSide * casterFootWidth -
                                shadowDir * casterFootDepth;

      const glm::vec2 casterC = spriteWorldSample +
                                shadowSide * casterFootWidth +
                                shadowDir * casterFootDepth;

      const glm::vec2 casterD = spriteWorldSample -
                                shadowSide * casterFootWidth +
                                shadowDir * casterFootDepth;

      auto buildShadowPolygon = [&](float length)
      {
        std::array<glm::vec2, 4> points = {
            casterA,
            casterB,
            casterC + shadowDir * length,
            casterD + shadowDir * length,
        };

        return points;
      };

      std::unordered_set<std::string> visitedGroundTiles;
      std::unordered_set<std::string> visitedWallFaces;
      std::unordered_set<std::string> visitedTraceWalls;

      const float verticalLight = std::max(std::abs(lightDir3D.z), 0.35f);

      //
      // PASS 0: calculate total climb cost first.
      //
      float effectiveShadowLength = shadowLength;
      float traceRemainingLength = shadowLength;
      int traceElevation = elevationLevel;
      glm::vec2 tracePosition = spriteWorldSample;

      for (int step = 0; step < MaxShadowSteps && traceRemainingLength > 0.0f;
           step++)
      {
        const float stepDistance =
            std::min(ShadowStepSize, traceRemainingLength);

        const glm::vec2 previousTracePosition = tracePosition;
        const glm::vec2 nextTracePosition =
            tracePosition + shadowDir * stepDistance;

        glm::vec2 stepShadowWorldPoints[4] = {
            previousTracePosition - shadowSide * casterFootWidth,
            previousTracePosition + shadowSide * casterFootWidth,
            nextTracePosition + shadowSide * casterFootWidth,
            nextTracePosition - shadowSide * casterFootWidth,
        };

        int minX = static_cast<int>(std::floor(stepShadowWorldPoints[0].x));
        int maxX = minX;
        int minY = static_cast<int>(std::floor(stepShadowWorldPoints[0].y));
        int maxY = minY;

        for (int i = 1; i < 4; i++)
        {
          minX = std::min(
              minX, static_cast<int>(std::floor(stepShadowWorldPoints[i].x)));
          maxX = std::max(
              maxX, static_cast<int>(std::floor(stepShadowWorldPoints[i].x)));
          minY = std::min(
              minY, static_cast<int>(std::floor(stepShadowWorldPoints[i].y)));
          maxY = std::max(
              maxY, static_cast<int>(std::floor(stepShadowWorldPoints[i].y)));
        }

        int highestHitElevationThisStep = traceElevation;

        auto testWall =
            [&](const glm::ivec2& wallTile, int wallElevation, int side)
        {
          if (wallElevation <= traceElevation)
            return;

          glm::vec2 wallA;
          glm::vec2 wallB;
          getWallEdge(wallTile, side, wallA, wallB);

          float minT = 0.0f;
          float maxT = 1.0f;

          if (!projectShadowOntoWallEdge(
                  stepShadowWorldPoints, wallA, wallB, minT, maxT))
            return;

          const std::string traceKey =
              tileKey(wallTile) + "," + std::to_string(side);

          if (visitedTraceWalls.contains(traceKey))
            return;

          visitedTraceWalls.insert(traceKey);

          highestHitElevationThisStep =
              std::max(highestHitElevationThisStep, wallElevation);
        };

        for (int y = minY; y <= maxY; y++)
        {
          for (int x = minX; x <= maxX; x++)
          {
            const glm::ivec2 wallTile{x, y};
            const int wallElevation = getTileElevationAt(glm::vec2{x, y});

            if (wallElevation <= traceElevation)
              continue;

            const int eastNeighborElevation =
                getTileElevationAt(glm::vec2{x + 1, y});

            const int southNeighborElevation =
                getTileElevationAt(glm::vec2{x, y + 1});

            if (eastNeighborElevation < wallElevation)
              testWall(wallTile, wallElevation, 2);

            if (southNeighborElevation < wallElevation)
              testWall(wallTile, wallElevation, 3);
          }
        }

        traceRemainingLength -= stepDistance;
        tracePosition = nextTracePosition;

        if (highestHitElevationThisStep > traceElevation)
        {
          const int elevationDelta =
              highestHitElevationThisStep - traceElevation;

          constexpr float ShadowClimbCostScale = 0.35f;

          const float elevationHeightInTiles =
              static_cast<float>(elevationStep) /
              (static_cast<float>(tileHeight) * worldScale);

          const float climbCost = static_cast<float>(elevationDelta) *
                                  elevationHeightInTiles *
                                  ShadowClimbCostScale / verticalLight;

          effectiveShadowLength -= climbCost;
          traceRemainingLength -= climbCost;
          traceElevation = highestHitElevationThisStep;

          if (traceRemainingLength <= 0.0f)
            break;
        }
      }

      effectiveShadowLength =
          std::clamp(effectiveShadowLength, 0.0f, shadowLength);

      auto finalShadowWorldPointsArray =
          buildShadowPolygon(effectiveShadowLength);

      glm::vec2 finalShadowWorldPoints[4] = {
          finalShadowWorldPointsArray[0],
          finalShadowWorldPointsArray[1],
          finalShadowWorldPointsArray[2],
          finalShadowWorldPointsArray[3],
      };

      //
      // PASS 1: ground/top faces using shortened final shadow.
      //
      {
        int minX = static_cast<int>(std::floor(finalShadowWorldPoints[0].x));
        int maxX = minX;
        int minY = static_cast<int>(std::floor(finalShadowWorldPoints[0].y));
        int maxY = minY;

        for (int i = 1; i < 4; i++)
        {
          minX = std::min(
              minX, static_cast<int>(std::floor(finalShadowWorldPoints[i].x)));
          maxX = std::max(
              maxX, static_cast<int>(std::floor(finalShadowWorldPoints[i].x)));
          minY = std::min(
              minY, static_cast<int>(std::floor(finalShadowWorldPoints[i].y)));
          maxY = std::max(
              maxY, static_cast<int>(std::floor(finalShadowWorldPoints[i].y)));
        }

        for (int y = minY; y <= maxY; y++)
        {
          for (int x = minX; x <= maxX; x++)
          {
            const glm::ivec2 shadowTile{x, y};
            const int tileElevation = getTileElevationAt(glm::vec2{x, y});

            const std::string groundKey =
                tileKey(shadowTile) + "," + std::to_string(tileElevation);

            if (visitedGroundTiles.contains(groundKey))
              continue;

            visitedGroundTiles.insert(groundKey);

            const float distanceAlongShadow = std::max(
                0.0f,
                glm::dot(glm::vec2{x + 0.5f, y + 0.5f} - spriteWorldSample,
                         shadowDir));

            const float normalizedDistance =
                std::clamp(distanceAlongShadow / shadowLength, 0.0f, 1.0f);

            constexpr float MinShadowFade = 0.65f;

            const float shadowFade =
                std::lerp(1.0f, MinShadowFade, normalizedDistance);

            const float groundAlpha = ShadowAlpha * shadowFade;

            submitTileShadowPolygonAt(
                shadowTile, tileElevation, finalShadowWorldPoints, groundAlpha);
          }
        }
      }

      //
      // PASS 2: exposed wall faces using same shortened final shadow.
      //
      {
        int minX = static_cast<int>(std::floor(finalShadowWorldPoints[0].x));
        int maxX = minX;
        int minY = static_cast<int>(std::floor(finalShadowWorldPoints[0].y));
        int maxY = minY;

        for (int i = 1; i < 4; i++)
        {
          minX = std::min(
              minX, static_cast<int>(std::floor(finalShadowWorldPoints[i].x)));
          maxX = std::max(
              maxX, static_cast<int>(std::floor(finalShadowWorldPoints[i].x)));
          minY = std::min(
              minY, static_cast<int>(std::floor(finalShadowWorldPoints[i].y)));
          maxY = std::max(
              maxY, static_cast<int>(std::floor(finalShadowWorldPoints[i].y)));
        }

        for (int y = minY; y <= maxY; y++)
        {
          for (int x = minX; x <= maxX; x++)
          {
            const glm::ivec2 wallTile{x, y};
            const int wallElevation = getTileElevationAt(glm::vec2{x, y});

            if (wallElevation <= 0)
              continue;

            const int eastNeighborElevation =
                getTileElevationAt(glm::vec2{x + 1, y});

            const int southNeighborElevation =
                getTileElevationAt(glm::vec2{x, y + 1});

            auto tryWall = [&](int side, int neighborElevation)
            {
              if (neighborElevation >= wallElevation)
                return;

              glm::vec2 wallA;
              glm::vec2 wallB;
              getWallEdge(wallTile, side, wallA, wallB);

              float minT = 0.0f;
              float maxT = 1.0f;

              if (!projectShadowOntoWallEdge(
                      finalShadowWorldPoints, wallA, wallB, minT, maxT))
                return;

              const std::string wallFaceKey =
                  tileKey(wallTile) + "," + std::to_string(side);

              if (visitedWallFaces.contains(wallFaceKey))
                return;

              visitedWallFaces.insert(wallFaceKey);

              const float distanceAlongShadow = std::max(
                  0.0f,
                  glm::dot(
                      ((wallA + wallB) * 0.5f) - spriteWorldSample, shadowDir));

              const float normalizedDistance =
                  std::clamp(distanceAlongShadow / shadowLength, 0.0f, 1.0f);

              constexpr float MinShadowFade = 0.65f;

              const float shadowFade =
                  std::lerp(1.0f, MinShadowFade, normalizedDistance);

              const float wallAlpha = ShadowAlpha * shadowFade;

              submitWallShadowFace(wallTile,
                                   wallElevation,
                                   side,
                                   finalShadowWorldPoints,
                                   static_cast<float>(neighborElevation),
                                   normalizedDistance,
                                   wallAlpha,
                                   item);
            };

            tryWall(2, eastNeighborElevation);
            tryWall(3, southNeighborElevation);
          }
        }
      }
    }

    submitSprite(item);
  }

  if (lightingSystem)
  {
    const glm::vec3 sunDir3D = lightingSystem->getLightDirection();

    const bool sunChanged =
        glm::length(sunDir3D - cachedTerrainShadowSunDir) > 0.025f;

    const bool terrainChanged =
        cachedTerrainShadowTileCount != tileElevationCache.size();

    const bool rebuildTerrainShadowCache =
        terrainShadowCacheDirty || sunChanged || terrainChanged;

    if (rebuildTerrainShadowCache)
    {
      cachedTerrainShadowItems.clear();

      const size_t before = renderItems.size();

      if (sunDir3D.z > 0.02f)
      {
        glm::vec2 shadowDir{-sunDir3D.x, -sunDir3D.y};

        if (glm::length(shadowDir) > 0.001f)
          shadowDir = glm::normalize(shadowDir);
        else
          shadowDir = glm::vec2{0.0f, 1.0f};

        const float sunHeight = std::max(sunDir3D.z, 0.08f);

        constexpr float TerrainShadowLengthScale = 1.35f;
        constexpr float TerrainShadowMaxLength = 3.0f;
        constexpr float TerrainShadowAlpha = 0.42f;

        const float alpha =
            TerrainShadowAlpha *
            std::clamp(lightingSystem->getDiffuseStrength(), 0.0f, 1.0f);

        if (alpha >= 0.04f)
        {
          const std::vector<TerrainShadowEdge> edges = getTerrainShadowEdges();

          for (const TerrainShadowEdge& edge : edges)
          {
            const int heightDelta = edge.topElevation - edge.bottomElevation;

            if (heightDelta <= 0)
              continue;

            const float shadowLength =
                std::min(TerrainShadowMaxLength,
                         static_cast<float>(heightDelta) *
                             TerrainShadowLengthScale / sunHeight);

            if (shadowLength < 0.15f)
              continue;

            submitTerrainEdgeShadowClipped(
                edge, shadowDir, shadowLength, alpha);
          }
        }
      }

      cachedTerrainShadowItems.assign(
          renderItems.begin() + before, renderItems.end());

      cachedTerrainShadowSunDir = sunDir3D;
      cachedTerrainShadowTileCount = tileElevationCache.size();
      terrainShadowCacheDirty = false;
    }
    else
    {
      renderItems.insert(renderItems.end(),
                         cachedTerrainShadowItems.begin(),
                         cachedTerrainShadowItems.end());
    }
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

  // Shadows live between ground tiles and sprites. They still keep a sort key
  // near their caster so they follow the caster's approximate depth.
  shadow.renderLayer = 1;

  const Uint8 a = static_cast<Uint8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);

  shadow.tint = SDL_Color{0, 0, 0, a};

  const float casterBottom = caster.screenSortY;
  const float shadowTipBottom = caster.screenSortY + shadowOffset.y;

  shadow.screenSortY = std::max(casterBottom, shadowTipBottom);
  shadow.sortKey = shadow.screenSortY + sortKeyBias;

  renderItems.push_back(shadow);
}

void IsometricRenderSystem::flushBatches()
{
  std::stable_sort(renderItems.begin(),
                   renderItems.end(),
                   [](const RenderItem& a, const RenderItem& b)
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

  for (const auto& item : renderItems)
  {
    auto& quadRenderer = RenderContext::quadRenderer();

    // if (item.isTerrainEdgeShadow)
    // {
    //   OpenGLQuadRenderer::TerrainShadowDrawCommand command;
    //
    //   const auto& activeCamera = getCamera();
    //   const float zoom = activeCamera.camera ? activeCamera.camera->zoom
    //   : 1.0f;
    //
    //   command.edgeA = item.terrainEdgeA;
    //   command.edgeB = item.terrainEdgeB;
    //   command.shadowDir = item.terrainShadowDir;
    //   command.shadowLength = item.terrainShadowLength;
    //   command.elevation = item.terrainShadowElevation;
    //
    //   command.zoom = zoom;
    //   command.isoCameraPosition = gridToIsometric(getCameraPosition());
    //   command.screenCenter = {
    //       static_cast<float>(windowWidth) * 0.5f,
    //       static_cast<float>(windowHeight) * 0.5f,
    //   };
    //
    //   command.tileWidth = tileWidth;
    //   command.tileHeight = tileHeight;
    //   command.elevationStep = elevationStep;
    //   command.worldScale = worldScale;
    //   command.tint = item.tint;
    //
    //   quadRenderer.drawTerrainShadow(command);
    //   continue;
    // }

    if (item.isTerrainShadow)
    {
      SDL_Surface* surface = assetStore.getSurface(*item.textureId);

      if (!surface)
        continue;

      const unsigned int texture =
          quadRenderer.getOrCreateTexture(*item.textureId, surface);

      if (texture == 0)
        continue;

      OpenGLQuadRenderer::FreeformQuadDrawCommand command;

      command.texture = texture;
      command.srcRect = item.srcRect;
      command.textureWidth = item.textureWidth;
      command.textureHeight = item.textureHeight;
      command.tint = item.tint;

      for (int i = 0; i < 4; i++)
        command.points[i] = item.shadowScreenPoints[i];

      quadRenderer.drawFreeformQuad(command);
      continue;
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

  // Sprites sample the tile they are standing on, not the neighboring raised
  // tile they may visually overlap. This prevents early visual "step up" while
  // approaching an elevated block.
  return glm::floor(transform.position);
}

bool IsometricRenderSystem::tryGetTileElevationAt(const glm::vec2& position,
                                                  int& outElevation) const
{
  const glm::ivec2 tile = gridCellOf(position);
  const std::string key = tileKey(tile);

  auto it = tileElevationCache.find(key);

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

void IsometricRenderSystem::submitTerrainShadow(const glm::vec2 screenPoints[4],
                                                SDL_Color tint,
                                                float sortKey,
                                                const std::string* textureId,
                                                SDL_Rect srcRect,
                                                int textureWidth,
                                                int textureHeight)
{
  RenderItem item;

  static const std::string shadowTextureId = "white_pixel";

  item.textureId = &shadowTextureId;
  item.srcRect = SDL_Rect{0, 0, 1, 1};
  item.textureWidth = 1;
  item.textureHeight = 1;

  item.isShadow = true;
  item.isTerrainShadow = true;
  item.tint = tint;
  item.sortKey = sortKey;
  item.renderLayer = 1;

  for (int i = 0; i < 4; i++)
    item.shadowScreenPoints[i] = screenPoints[i];

  renderItems.push_back(item);
}
} // namespace sfs
