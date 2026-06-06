#include "engine/runtime/rendering/modules/terrainShadow.h"
#include "engine/core/components/tags/isometricTile.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/logger/logger.h"
#include "engine/runtime/rendering/util/isometric/geometry.h"
#include "engine/runtime/systems/isometric/isometricRenderSystem.h"
#include "engine/core/util/algorithms/gridDDA.h"
#include "engine/runtime/rendering/util/isometric/isometricLightingUtils.h"
#include "engine/core/util/parallelFor.h"

#include <algorithm>
#include <vector>

#include "engine/core/components/elevationComponent.h"
#include "engine/core/components/terrainBoundaryComponent.h"
#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"

#include "engine/core/util/profiling.h"

namespace sfs
{

std::atomic<uint64_t> gShadowPathChecks{0};
std::atomic<uint64_t> gShadowTilesTraversed{0};

void TerrainShadow::markTerrainDirty()
{
  m_cache.edgesDirty = true;
  m_cache.itemsDirty = true;
}

void TerrainShadow::computeCommands(const IsometricRenderContext& context)
{
  ZoneScopedN("ShadowSystem: computeCommands");

  if (!context.ambientLighting)
  {
    flush();
    m_cache.itemsDirty = true;
    return;
  }

  computeTerrainShadows(context);
}

void TerrainShadow::computeTerrainShadows(const IsometricRenderContext& context)
{
  ZoneScopedN("Compute terrain shadows");

  const auto* ambientLighting = context.ambientLighting;

  if (m_cache.edgesDirty)
  {
    m_cache.edges = mergeTerrainShadowEdges(getTerrainShadowEdges(context));
    m_cache.edgeTileBounds = getTerrainShadowEdgeTileBounds(m_cache.edges);

    m_cache.edgesDirty = false;
    m_cache.itemsDirty = true;
  }

  if (!ambientLighting)
    return;

  const glm::vec3 sunDir3D = ambientLighting->direction;

  if (sunDir3D.z <= 0.02f)
  {
    flush();
    m_cache.itemsDirty = true;
    m_cache.sunDir = sunDir3D;
    return;
  }

  glm::vec2 shadowDir{-sunDir3D.x, -sunDir3D.y};

  const float horizontalAmount = glm::length(shadowDir);

  if (horizontalAmount <= 0.001f)
  {
    flush();
    m_cache.itemsDirty = true;
    m_cache.sunDir = sunDir3D;
    return;
  }

  shadowDir /= horizontalAmount;

  const float sunHeight = glm::max(sunDir3D.z, 0.08f);
  const float effectiveSunHeight =
      sunHeight / glm::max(horizontalAmount, 0.001f);

  const float diffuse =
      glm::clamp(ambientLighting->diffuseStrength, 0.0f, 1.0f);

  if (diffuse <= 0.01f)
  {
    flush();
    m_cache.itemsDirty = true;
    return;
  }

  const float alpha = m_shadowSettings.terrainShadowAlpha * diffuse;

  if (alpha < 0.04f)
  {
    flush();
    m_cache.itemsDirty = true;
    return;
  }

  const bool sunChanged = glm::length(sunDir3D - m_cache.sunDir) > 0.002f;

  const auto isoCameraPosition = context.projection->cameraIso;
  const auto zoom = context.projection->zoom;

  const bool cameraChanged =
      glm::length(isoCameraPosition - m_cache.isoCameraPosition) > 0.001f ||
      glm::abs(zoom - m_cache.zoom) > 0.001f;
  const bool alphaChanged = glm::abs(alpha - m_cache.alpha) > 0.005f;

  if (!m_cache.itemsDirty && !sunChanged && !cameraChanged && !alphaChanged)
  {
    return;
  }

  // The shadow build indexes the elevation grid directly; an invalid grid would
  // read out of bounds, so skip the rebuild this frame.
  if (!context.terrainElevationGrid.valid())
  {
    LOG_ERROR(
        "terrain shadow: elevation grid invalid; skipping shadow rebuild");
    return;
  }

  gShadowPathChecks.store(0, std::memory_order_relaxed);
  gShadowTilesTraversed.store(0, std::memory_order_relaxed);

  flush();
  buildTerrainEdgeShadowItems(context, shadowDir, effectiveSunHeight, alpha);

  m_cache.sunDir = sunDir3D;
  m_cache.isoCameraPosition = isoCameraPosition;
  m_cache.zoom = zoom;
  m_cache.itemsDirty = false;
  m_cache.alpha = alpha;
}

std::vector<TerrainShadowEdge> TerrainShadow::getTerrainShadowEdges(
    const IsometricRenderContext& context) const
{
  std::vector<TerrainShadowEdge> edges;

  auto tiles = registry->view<TransformComponent,
                              TerrainBoundaryComponent,
                              IsometricTile,
                              ElevationComponent>();

  for (const auto& entity : tiles)
  {
    const auto& transform = entity.getComponent<TransformComponent>();
    const auto& boundary = entity.getComponent<TerrainBoundaryComponent>();
    const int elevation = entity.getComponent<ElevationComponent>().level;

    const glm::ivec2 tile = context.gridCellOf(transform.position);

    const int x = tile.x;
    const int y = tile.y;

    if (boundary.westExposed && boundary.westBottomElevation)
    {
      edges.push_back({
          glm::vec2{x, y},
          glm::vec2{x, y + 1},
          tile,
          glm::ivec2{x - 1, y},
          elevation,
          boundary.westBottomElevation,
          TerrainShadowEdge::Side::West,
      });
    }

    if (boundary.eastExposed && boundary.eastBottomElevation)
    {
      edges.push_back({
          glm::vec2{x + 1, y},
          glm::vec2{x + 1, y + 1},
          tile,
          glm::ivec2{x + 1, y},
          elevation,
          boundary.eastBottomElevation,
          TerrainShadowEdge::Side::East,
      });
    }

    if (boundary.northExposed && boundary.northBottomElevation)
    {
      edges.push_back({
          glm::vec2{x, y},
          glm::vec2{x + 1, y},
          tile,
          glm::ivec2{x, y - 1},
          elevation,
          boundary.northBottomElevation,
          TerrainShadowEdge::Side::North,
      });
    }

    if (boundary.southExposed && boundary.southBottomElevation)
    {
      edges.push_back({
          glm::vec2{x, y + 1},
          glm::vec2{x + 1, y + 1},
          tile,
          glm::ivec2{x, y + 1},
          elevation,
          boundary.southBottomElevation,
          TerrainShadowEdge::Side::South,
      });
    }
  }

  return edges;
}

void TerrainShadow::emitTileShadow(const IsometricRenderContext& context,
                                   std::vector<Quad>& outQuads,
                                   const glm::ivec2& tile,
                                   int elevation,
                                   const ClippedPolygon& poly,
                                   float alpha)
{
  if (poly.count < 3)
    return;

  constexpr float ElevationSortWeight = 0.5f;

  const float elevationF = static_cast<float>(elevation);

  // Painter sort-key, baked into each quad's z; assignClipDepth() remaps it to
  // clip-space depth so each shadow tile occludes correctly via the depth
  // buffer (one batch, no per-depth grouping needed).
  const float sortKey = static_cast<float>(tile.x) +
                        static_cast<float>(tile.y) +
                        elevationF * ElevationSortWeight + 0.0005f;

  const SDL_Color tint{
      0,
      0,
      0,
      static_cast<Uint8>(glm::clamp(alpha, 0.0f, 1.0f) * 255.0f),
  };

  glm::vec2 screenPoly[8];

  for (int i = 0; i < poly.count; i++)
    screenPoly[i] = context.worldToScreen(poly.points[i], elevationF);

  for (int i = 1; i + 1 < poly.count; i++)
  {
    Quad quad;
    quad.tint = tint;
    quad.points[0] = screenPoly[0];
    quad.points[1] = screenPoly[i];
    quad.points[2] = screenPoly[i + 1];
    quad.points[3] = screenPoly[i + 1];
    quad.z = sortKey;

    outQuads.push_back(quad);
  }
}

void TerrainShadow::constructTerrainEdgeShadowProjectedClipped(
    const IsometricRenderContext& context,
    std::vector<Quad>& outQuads,
    const TerrainShadowEdge& edge,
    const glm::vec2& shadowDir,
    float sunHeight,
    float maxShadowLength,
    float alpha)
{
  ZoneScopedN("Shadow: constructTerrainEdgeShadowProjectedClipped()");

  if (sunHeight <= 0.001f || maxShadowLength <= 0.05f || alpha <= 0.01f)
    return;

  const float shadowLength =
      calculateTerrainShadowLength(edge, sunHeight, maxShadowLength);

  if (shadowLength <= 0.05f)
    return;

  glm::vec2 shadowWorldPoints[4] = {
      edge.a,
      edge.b,
      edge.b + shadowDir * shadowLength,
      edge.a + shadowDir * shadowLength,
  };

  constexpr int ReceiverBoundsPadding = 8;

  const TileBounds receiverBounds =
      expandedTileBounds(m_cache.edgeTileBounds, ReceiverBoundsPadding);

  if (!shadowQuadOverlapsTileBounds(shadowWorldPoints, receiverBounds))
    return;

  {
    ZoneScopedN("Shadow: emitTileShadow()");
    forEachTileOverlappingShadowQuad(
        shadowWorldPoints,
        shadowDir,
        [&](const glm::ivec2& tile, const ClippedPolygon& poly)
        {
          int elevation = 0;

          if (!context.terrainElevationGrid.tryGet(tile, elevation))
            return true;

          if (elevation > edge.bottomElevation)
            return true;

          emitTileShadow(context, outQuads, tile, elevation, poly, alpha);

          return true;
        });
  }
}

bool TerrainShadow::terrainShadowPathBlocked(
    const IsometricRenderContext& context,
    const TerrainShadowEdge& edge,
    const glm::ivec2& receiverTile,
    const glm::vec2& shadowDir) const
{
  ZoneScopedN("terrainShadowPathBlocked()")

#if !defined(NDEBUG)
      gShadowPathChecks.fetch_add(1, std::memory_order_relaxed);
#endif

  const glm::vec2 start = (edge.a + edge.b) * 0.5f + shadowDir * 0.02f;

  const glm::vec2 target{
      receiverTile.x + 0.5f,
      receiverTile.y + 0.5f,
  };

  const glm::vec2 delta = target - start;
  const float distance = glm::length(delta);

  if (distance <= 0.001f)
    return false;

  const glm::vec2 dir = delta / distance;

  if (glm::dot(dir, shadowDir) <= 0.5f)
    return false;

  bool blocked = false;

  walkGridDDA(start,
              dir,
              distance,
              [&](const glm::ivec2& tile, float)
              {
#if !defined(NDEBUG)
                gShadowTilesTraversed.fetch_add(1, std::memory_order_relaxed);
#endif

                if (tile == receiverTile)
                  return false;

                int elevation = 0;

                if (!context.terrainElevationGrid.tryGet(tile, elevation))
                {
                  blocked = true;
                  return false;
                }

                if (elevation >= edge.topElevation)
                {
                  blocked = true;
                  return false;
                }

                return true;
              });

  return blocked;
}

bool TerrainShadow::shouldCastTerrainShadow(const TerrainShadowEdge& edge,
                                            const glm::vec2& shadowDir)
{
  switch (edge.side)
  {
  case TerrainShadowEdge::Side::West:
    return shadowDir.x < -0.05f;

  case TerrainShadowEdge::Side::East:
    return shadowDir.x > 0.05f;

  case TerrainShadowEdge::Side::North:
    return shadowDir.y < -0.05f;

  case TerrainShadowEdge::Side::South:
    return shadowDir.y > 0.05f;
  }

  return false;
}

std::vector<TerrainShadowEdge> TerrainShadow::mergeTerrainShadowEdges(
    const std::vector<TerrainShadowEdge>& input) const
{
  ZoneScopedN("mergeTerrainShadowEdges()") struct Run
  {
    TerrainShadowEdge edge;
    int constant = 0;
    int start = 0;
    int end = 0;
    bool vertical = false;
  };

  std::vector<Run> runs;
  runs.reserve(input.size());

  for (const TerrainShadowEdge& edge : input)
  {
    const bool vertical = glm::abs(edge.a.x - edge.b.x) < 0.001f;

    Run run;
    run.edge = edge;
    run.vertical = vertical;

    if (vertical)
    {
      run.constant = static_cast<int>(glm::round(edge.a.x));
      run.start = static_cast<int>(glm::round(glm::min(edge.a.y, edge.b.y)));
      run.end = static_cast<int>(glm::round(glm::max(edge.a.y, edge.b.y)));
    }
    else
    {
      run.constant = static_cast<int>(glm::round(edge.a.y));
      run.start = static_cast<int>(glm::round(glm::min(edge.a.x, edge.b.x)));
      run.end = static_cast<int>(glm::round(glm::max(edge.a.x, edge.b.x)));
    }

    runs.push_back(run);
  }

  std::sort(runs.begin(),
            runs.end(),
            [](const Run& a, const Run& b)
            {
              if (a.edge.side != b.edge.side)
                return a.edge.side < b.edge.side;

              if (a.edge.topElevation != b.edge.topElevation)
                return a.edge.topElevation < b.edge.topElevation;

              if (a.edge.bottomElevation != b.edge.bottomElevation)
                return a.edge.bottomElevation < b.edge.bottomElevation;

              return a.start < b.start;
            });

  std::vector<TerrainShadowEdge> merged;

  for (const Run& run : runs)
  {
    if (merged.empty())
    {
      merged.push_back(run.edge);
      continue;
    }

    TerrainShadowEdge& last = merged.back();

    const bool lastVertical = glm::abs(last.a.x - last.b.x) < 0.001f;

    bool canMerge = lastVertical == run.vertical &&
                    last.topElevation == run.edge.topElevation &&
                    last.bottomElevation == run.edge.bottomElevation &&
                    last.side == run.edge.side;

    if (canMerge)
    {
      if (run.vertical)
      {
        const int lastX = static_cast<int>(glm::round(last.a.x));
        const int lastEnd =
            static_cast<int>(glm::round(glm::max(last.a.y, last.b.y)));

        canMerge = lastX == run.constant && lastEnd == run.start;
      }
      else
      {
        const int lastY = static_cast<int>(glm::round(last.a.y));
        const int lastEnd =
            static_cast<int>(glm::round(glm::max(last.a.x, last.b.x)));

        canMerge = lastY == run.constant && lastEnd == run.start;
      }
    }

    if (!canMerge)
    {
      merged.push_back(run.edge);
      continue;
    }

    if (run.vertical)
    {
      last.b.y = run.edge.b.y;
      last.receiverTile = run.edge.receiverTile;
    }
    else
    {
      last.b.x = run.edge.b.x;
      last.receiverTile = run.edge.receiverTile;
    }
  }

  return merged;
}

void TerrainShadow::setTerrainShadowMaxLength(float length)
{
  m_shadowSettings.terrainShadowMaxLength = glm::max(length, 0.0f);

  markTerrainDirty();
}

void TerrainShadow::setTerrainShadowAlpha(float alpha)
{
  m_shadowSettings.terrainShadowAlpha = glm::clamp(alpha, 0.0f, 1.0f);

  markTerrainDirty();
}

float TerrainShadow::calculateTerrainShadowLength(const TerrainShadowEdge& edge,
                                                  float sunHeight,
                                                  float maxShadowLength)
{
  // sunHeight here is the effective sun height (sun height / horizontal
  // amount), so its reciprocal is the horizontal reach per unit height. Shared
  // with sprite shadows so an edge and a sprite of the same height match.
  const float heightDelta =
      static_cast<float>(edge.topElevation - edge.bottomElevation);

  return projectedShadowLength(
      heightDelta, 1.0f / glm::max(sunHeight, 0.001f), maxShadowLength);
}

void TerrainShadow::buildTerrainEdgeShadowItems(
    const IsometricRenderContext& context,
    const glm::vec2& shadowDir,
    float sunHeight,
    float alpha)
{
  ZoneScopedN("Shadow: buildTerrainEdgeShadowItems()");

  const auto& edges = m_cache.edges;

  m_commands.clear();

  if (edges.empty())
    return;

  const float maxLength = m_shadowSettings.terrainShadowMaxLength;

  // Each edge projects its shadow independently, so fan the edge loop across
  // the worker pool. Every range writes into its own persistent buffer (no
  // shared mutation, no per-frame allocation); each quad carries its own world
  // depth in z, so the results just concatenate into one batch with no sort or
  // per-depth grouping.
  const std::size_t rangeCount = parallelForRangeCount(edges.size());

  m_shadowRangeQuads.resize(rangeCount);
  for (std::vector<Quad>& buffer : m_shadowRangeQuads)
    buffer.clear();

  std::vector<int> rangeEdges(rangeCount, 0);

  parallelFor(
      edges.size(),
      [&](std::size_t begin, std::size_t end, std::size_t range)
      {
        std::vector<Quad>& out = m_shadowRangeQuads[range];

        int processed = 0;

        for (std::size_t i = begin; i < end; i++)
        {
          const TerrainShadowEdge& edge = edges[i];

          if (edge.topElevation <= edge.bottomElevation)
            continue;

          if (!shouldCastTerrainShadow(edge, shadowDir))
            continue;

          constructTerrainEdgeShadowProjectedClipped(
              context, out, edge, shadowDir, sunHeight, maxLength, alpha);

          processed++;
        }

        rangeEdges[range] = processed;
      });

  std::size_t quadCount = 0;
  for (const std::vector<Quad>& buffer : m_shadowRangeQuads)
    quadCount += buffer.size();

  for (int processed : rangeEdges)
    gTerrainShadowEdgesProcessed += processed;

  if (quadCount == 0)
    return;

  // All shadow quads go into a single batch (each carries its own depth in z),
  // submitted together through the one stencil-deduped flush.
  TerrainShadowBatchCommand batch;
  batch.quad.quads.reserve(quadCount);

  for (std::vector<Quad>& buffer : m_shadowRangeQuads)
    batch.quad.quads.insert(
        batch.quad.quads.end(), buffer.begin(), buffer.end());

  m_commands.push_back(std::move(batch));
}
} // namespace sfs
