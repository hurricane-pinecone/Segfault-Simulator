#include "engine/systems/isometric/isometricShadowSystem.h"
#include "engine/components/transformComponent.h"
#include "engine/renderers/util/isometric/geometry.h"
#include "engine/systems/isometric/isometricRenderSystem.h"
#include "engine/utils/algorithms/gridDDA.h"
#include "engine/utils/isometricLightingUtils.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <vector>
#ifdef __EMSCRIPTEN__
  #include <chrono>
#endif
#include <cmath>
#include <future>
#include <thread>

#include "engine/components/elevationComponent.h"
#include "engine/components/tags/isometricTile.h"
#include "engine/components/terrainBoundaryComponent.h"
#include "glm/glm/geometric.hpp"

#include "engine/ecs/registry.h" // IWYU pragma: keep

namespace sfs
{

std::atomic<uint64_t> gShadowPathChecks{0};
std::atomic<uint64_t> gShadowTilesTraversed{0};

IsometricShadowSystem::IsometricShadowSystem()
{
  registerComponent<TransformComponent>();
  registerComponent<TerrainBoundaryComponent>();
}

void IsometricShadowSystem::markTerrainDirty()
{
  m_cache.edgesDirty = true;
  m_cache.itemsDirty = true;
}

void IsometricShadowSystem::setAmbientLighting(
    const IsometricAmbientLighting* ambient)
{
  m_ambientLighting = ambient;
}

void IsometricShadowSystem::computeCommands(
    const IsometricRenderContext& context)
{
  if (!m_ambientLighting)
  {
    flush();
    m_cache.itemsDirty = true;
    return;
  }

  computeTerrainShadows(context);
}

void IsometricShadowSystem::computeTerrainShadows(
    const IsometricRenderContext& context)
{
  ZoneScopedN("Compute terrain shadows");
  if (m_cache.edgesDirty)
  {
    m_cache.edges = mergeTerrainShadowEdges(getTerrainShadowEdges(context));

    m_cache.edgesDirty = false;
    m_cache.itemsDirty = true;
  }

  if (!m_ambientLighting)
    return;

  const glm::vec3 sunDir3D = m_ambientLighting->direction;

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

  const float sunHeight = std::max(sunDir3D.z, 0.08f);
  const float effectiveSunHeight =
      sunHeight / std::max(horizontalAmount, 0.001f);

  const float diffuse =
      std::clamp(m_ambientLighting->diffuseStrength, 0.0f, 1.0f);

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

  const bool sunChanged = glm::length(sunDir3D - m_cache.sunDir) > 0.025f;

  const bool cameraChanged = glm::length(context.isoCameraPosition -
                                         m_cache.isoCameraPosition) > 0.001f ||
                             std::abs(context.zoom - m_cache.zoom) > 0.001f;
  const bool alphaChanged = std::abs(alpha - m_cache.alpha) > 0.005f;

  if (!m_cache.itemsDirty && !sunChanged && !cameraChanged && !alphaChanged)
  {
    return;
  }

  gShadowPathChecks.store(0, std::memory_order_relaxed);
  gShadowTilesTraversed.store(0, std::memory_order_relaxed);

  // Multi threaded
#ifdef __EMSCRIPTEN__
  if (m_shadowBuildInProgress)
  {
    bool allReady = true;

    for (auto& job : m_shadowJobs)
    {
      if (job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      {
        allReady = false;
        break;
      }
    }

    if (allReady)
    {
      std::vector<TerrainShadowCommand> newItems;

      for (auto& job : m_shadowJobs)
      {
        ShadowBuildResult result = job.get();

        gTerrainShadowEdgesProcessed += result.edgesProcessed;

        newItems.insert(
            newItems.end(), result.items.begin(), result.items.end());
      }

      m_commands = batchTerrainShadowCommands(newItems);
      m_cache.sunDir = sunDir3D;
      m_cache.isoCameraPosition = context.isoCameraPosition;
      m_cache.zoom = context.zoom;
      m_cache.itemsDirty = false;
      m_cache.alpha = alpha;

      m_shadowJobs.clear();
      m_shadowBuildInProgress = false;
    }

    return;
  }

  m_shadowJobs =
      startTerrainEdgeShadowJobs(context, shadowDir, effectiveSunHeight, alpha);

  m_shadowBuildInProgress = !m_shadowJobs.empty();

  if (!m_shadowBuildInProgress)
    flush();

  return;
#else
  flush();
  buildTerrainEdgeShadowItems(context, shadowDir, effectiveSunHeight, alpha);
#endif

  m_cache.sunDir = sunDir3D;
  m_cache.isoCameraPosition = context.isoCameraPosition;
  m_cache.zoom = context.zoom;
  m_cache.itemsDirty = false;
  m_cache.alpha = alpha;
}

std::vector<TerrainShadowEdge> IsometricShadowSystem::getTerrainShadowEdges(
    const IsometricRenderContext& context) const
{
  std::vector<TerrainShadowEdge> edges;

  for (const auto& entity : getEntities())
  {
    if (!entity.hasComponent<IsometricTile>())
      continue;

    if (!entity.hasComponent<TerrainBoundaryComponent>())
      continue;

    const auto& transform = entity.getComponent<TransformComponent>();
    const auto& boundary = entity.getComponent<TerrainBoundaryComponent>();

    int elevation = 0;

    if (entity.hasComponent<ElevationComponent>())
      elevation = entity.getComponent<ElevationComponent>().level;

    if (elevation <= 0)
      continue;

    const glm::ivec2 tile = context.gridCellOf(transform.position);

    const int x = tile.x;
    const int y = tile.y;

    if (boundary.westExposed)
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

    if (boundary.eastExposed)
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

    if (boundary.northExposed)
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

    if (boundary.southExposed)
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

void IsometricShadowSystem::constructTileShadowPolygonAt(
    const IsometricRenderContext& context,
    std::vector<TerrainShadowCommand>& outCommands,
    const glm::ivec2& tile,
    int elevation,
    const glm::vec2 worldPoints[4],
    float alpha)
{
  ZoneScopedN("Shadow: constructTileShadowPolygonAt()");
  const auto clipped = clipPolygonToTile(worldPoints, tile);

  if (clipped.count < 3)
    return;

  constexpr float ElevationSortWeight = 0.5f;

  const float sortKey =
      static_cast<float>(tile.x) + static_cast<float>(tile.y) +
      static_cast<float>(elevation) * ElevationSortWeight + 0.0005f;

  for (size_t i = 1; i + 1 < clipped.count; i++)
  {
    glm::vec2 screenPoints[4] = {
        context.worldToScreen(clipped.points[0], static_cast<float>(elevation)),
        context.worldToScreen(clipped.points[i], static_cast<float>(elevation)),
        context.worldToScreen(
            clipped.points[i + 1], static_cast<float>(elevation)),
        context.worldToScreen(
            clipped.points[i + 1], static_cast<float>(elevation)),
    };

    outCommands.push_back(
        buildTerrainShadowCommand(screenPoints, alpha, sortKey));
  }
}

void IsometricShadowSystem::constructTerrainEdgeShadowProjectedClipped(
    const IsometricRenderContext& context,
    std::vector<TerrainShadowCommand>& outCommands,
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

  forEachTileOverlappingShadowQuad(shadowWorldPoints,
                                   [&](const glm::ivec2& tile, float)
                                   {
                                     tryConstructShadowOnTile(
                                         context,
                                         outCommands,
                                         tile,
                                         edge.bottomElevation,
                                         shadowWorldPoints,
                                         alpha);

                                     return true;
                                   });
}

bool IsometricShadowSystem::terrainShadowPathBlocked(
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

                if (!context.hasTileAt(tile))
                {
                  blocked = true;
                  return false;
                }

                const int elevation =
                    context.getTileElevationAt(glm::vec2{tile.x, tile.y});

                if (elevation >= edge.topElevation)
                {
                  blocked = true;
                  return false;
                }

                return true;
              });

  return blocked;
}

bool IsometricShadowSystem::shouldCastTerrainShadow(
    const TerrainShadowEdge& edge,
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

void IsometricShadowSystem::constructWallShadowFace(
    const IsometricRenderContext& renderContext,
    std::vector<TerrainShadowCommand>& outCommands,
    const glm::ivec2& tile,
    int elevation,
    int side,
    const glm::vec2 shadowWorldPoints[4],
    float incomingElevation,
    float normalizedDistance,
    float alpha)
{
  ZoneScopedN("constructWallShadowFace()");
  glm::vec2 a;
  glm::vec2 b;
  getTileWallEdge(tile, side, a, b);

  float minT = 0.0f;
  float maxT = 1.0f;

  if (!projectShadowOntoWallEdge(shadowWorldPoints, a, b, minT, maxT))
    return;

  constexpr float WallPad = 0.05f;

  minT = std::clamp(minT - WallPad, 0.0f, 1.0f);
  maxT = std::clamp(maxT + WallPad, 0.0f, 1.0f);

  const glm::vec2 shadowA = a + (b - a) * minT;
  const glm::vec2 shadowB = a + (b - a) * maxT;

  const float topZ = static_cast<float>(elevation);
  const float incomingZ = std::clamp(incomingElevation, 0.0f, topZ);

  if (topZ <= incomingZ + 0.001f)
    return;

  glm::vec2 screenPoints[4] = {
      renderContext.worldToScreen(shadowA, topZ),
      renderContext.worldToScreen(shadowB, topZ),
      renderContext.worldToScreen(shadowB, incomingZ),
      renderContext.worldToScreen(shadowA, incomingZ),
  };

  const float sortKey = static_cast<float>(tile.x) +
                        static_cast<float>(tile.y) +
                        static_cast<float>(elevation) * 0.5f + 0.0006f;

  outCommands.push_back(
      buildTerrainShadowCommand(screenPoints, alpha, sortKey));
}

std::vector<TerrainShadowEdge> IsometricShadowSystem::mergeTerrainShadowEdges(
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
    const bool vertical = std::abs(edge.a.x - edge.b.x) < 0.001f;

    Run run;
    run.edge = edge;
    run.vertical = vertical;

    if (vertical)
    {
      run.constant = static_cast<int>(std::round(edge.a.x));
      run.start = static_cast<int>(std::round(std::min(edge.a.y, edge.b.y)));
      run.end = static_cast<int>(std::round(std::max(edge.a.y, edge.b.y)));
    }
    else
    {
      run.constant = static_cast<int>(std::round(edge.a.y));
      run.start = static_cast<int>(std::round(std::min(edge.a.x, edge.b.x)));
      run.end = static_cast<int>(std::round(std::max(edge.a.x, edge.b.x)));
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

    const bool lastVertical = std::abs(last.a.x - last.b.x) < 0.001f;

    bool canMerge = lastVertical == run.vertical &&
                    last.topElevation == run.edge.topElevation &&
                    last.bottomElevation == run.edge.bottomElevation &&
                    last.side == run.edge.side;

    if (canMerge)
    {
      if (run.vertical)
      {
        const int lastX = static_cast<int>(std::round(last.a.x));
        const int lastEnd =
            static_cast<int>(std::round(std::max(last.a.y, last.b.y)));

        canMerge = lastX == run.constant && lastEnd == run.start;
      }
      else
      {
        const int lastY = static_cast<int>(std::round(last.a.y));
        const int lastEnd =
            static_cast<int>(std::round(std::max(last.a.x, last.b.x)));

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

void IsometricShadowSystem::buildTerrainEdgeShadowItems(
    const IsometricRenderContext& context,
    const glm::vec2& shadowDir,
    float sunHeight,
    float alpha)
{
  ZoneScopedN("Shadow: buildTerrainEdgeShadowItems()");

  const std::vector<TerrainShadowEdge>& edges = m_cache.edges;
  const std::size_t edgeCount = edges.size();
  std::vector<TerrainShadowCommand> rawItems;
  rawItems.reserve(5000);

  struct ShadowBuildResult
  {
    std::vector<TerrainShadowCommand> items;
    int edgesProcessed = 0;
  };

  if (edgeCount == 0)
    return;

#ifdef __EMSCRIPTEN__
  const unsigned int hardwareThreads = 2;
#else
  const unsigned int hardwareThreads =
      std::max(1u, std::min(4u, std::thread::hardware_concurrency()));
#endif

  const std::size_t batchCount =
      std::min<std::size_t>(hardwareThreads, edgeCount);

  const std::size_t batchSize = (edgeCount + batchCount - 1) / batchCount;

  std::vector<std::future<ShadowBuildResult>> jobs;
  jobs.reserve(batchCount);

  for (std::size_t batchIndex = 0; batchIndex < batchCount; batchIndex++)
  {
    const std::size_t begin = batchIndex * batchSize;
    const std::size_t end = std::min(begin + batchSize, edgeCount);

    if (begin >= end)
      continue;

    jobs.push_back(std::async(
        std::launch::async,
        [this, &context, &edges, shadowDir, sunHeight, alpha, begin, end]()
        {
          ShadowBuildResult result;
          result.items.reserve((end - begin) * 8);

          for (std::size_t i = begin; i < end; i++)
          {
            const TerrainShadowEdge& edge = edges[i];

            const int heightDelta = edge.topElevation - edge.bottomElevation;

            if (heightDelta <= 0)
              continue;

            if (!shouldCastTerrainShadow(edge, shadowDir))
              continue;

            constructTerrainEdgeShadowProjectedClipped(
                context,
                result.items,
                edge,
                shadowDir,
                sunHeight,
                m_shadowSettings.terrainShadowMaxLength,
                alpha);

            result.edgesProcessed++;
          }

          return result;
        }));
  }

  for (auto& job : jobs)
  {
    ShadowBuildResult result = job.get();

    gTerrainShadowEdgesProcessed += result.edgesProcessed;

    rawItems.insert(rawItems.end(), result.items.begin(), result.items.end());
  }
  m_commands = batchTerrainShadowCommands(rawItems);
}

void IsometricShadowSystem::setTerrainShadowMaxLength(float length)
{
  m_shadowSettings.terrainShadowMaxLength = std::max(length, 0.0f);

  markTerrainDirty();
}

void IsometricShadowSystem::setTerrainShadowAlpha(float alpha)
{
  m_shadowSettings.terrainShadowAlpha = std::clamp(alpha, 0.0f, 1.0f);

  markTerrainDirty();
}

bool IsometricShadowSystem::tryConstructShadowOnTile(
    const IsometricRenderContext& context,
    std::vector<TerrainShadowCommand>& outCommands,
    const glm::ivec2& tile,
    int requiredElevation,
    const glm::vec2 worldPoints[4],
    float alpha)
{

  ZoneScopedN("Shadow: tryConstructShadowOnTile()");
  if (!context.hasTileAt(tile))
    return false;

  const int receiverElevation =
      context.getTileElevationAt(glm::vec2{tile.x, tile.y});

  if (receiverElevation != requiredElevation)
    return false;

  constructTileShadowPolygonAt(
      context, outCommands, tile, receiverElevation, worldPoints, alpha);

  return true;
}

float IsometricShadowSystem::calculateTerrainShadowLength(
    const TerrainShadowEdge& edge,
    float sunHeight,
    float maxShadowLength)
{

  ZoneScopedN("Shadow: calculateTerrainShadowLength()");
  const int heightDelta = edge.topElevation - edge.bottomElevation;

  if (heightDelta <= 0)
    return 0.0f;

  constexpr float TerrainShadowLengthScale = 1.35f;

  return std::min(
      maxShadowLength,
      static_cast<float>(heightDelta) * TerrainShadowLengthScale / sunHeight);
}

TerrainShadowCommand IsometricShadowSystem::buildTerrainShadowCommand(
    const glm::vec2 screenPoints[4],
    float alpha,
    float sortKey)
{
  TerrainShadowCommand item;
  item.order.depth = sortKey;
  item.quad.tint = SDL_Color{
      0,
      0,
      0,
      static_cast<Uint8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f),
  };
  for (int i = 0; i < 4; i++)
    item.quad.points[i] = screenPoints[i];
  return item;
}

std::vector<TerrainShadowBatchCommand>
IsometricShadowSystem::batchTerrainShadowCommands(
    const std::vector<TerrainShadowCommand>& items) const
{
  std::map<RenderOrderKey, std::vector<Quad>> batches;

  for (const auto& item : items)
  {
    RenderOrderKey key{
        item.order.pass,
        item.order.depth,
        item.order.subpass,
    };

    batches[key].push_back(item.quad);
  }

  std::vector<TerrainShadowBatchCommand> result;
  result.reserve(batches.size());

  for (auto& [key, quads] : batches)
  {
    TerrainShadowBatchCommand batch;
    batch.order = {key.pass, key.depth, key.subpass};
    batch.quad.quads = std::move(quads);
    result.push_back(std::move(batch));
  }

  return result;
}

#ifdef __EMSCRIPTEN__
std::vector<std::future<IsometricShadowSystem::ShadowBuildResult>>
IsometricShadowSystem::startTerrainEdgeShadowJobs(
    const IsometricRenderContext& context,
    const glm::vec2& shadowDir,
    float sunHeight,
    float alpha)
{
  const auto edges =
      std::make_shared<const std::vector<TerrainShadowEdge>>(m_cache.edges);
  const auto contextCopy =
      std::make_shared<const IsometricRenderContext>(context);
  const std::size_t edgeCount = edges->size();

  constexpr unsigned int hardwareThreads = 2;

  std::vector<std::future<ShadowBuildResult>> jobs;

  if (edgeCount == 0)
    return jobs;

  const std::size_t batchCount =
      std::min<std::size_t>(hardwareThreads, edgeCount);

  const std::size_t batchSize = (edgeCount + batchCount - 1) / batchCount;

  jobs.reserve(batchCount);

  for (std::size_t batchIndex = 0; batchIndex < batchCount; batchIndex++)
  {
    const std::size_t begin = batchIndex * batchSize;
    const std::size_t end = std::min(begin + batchSize, edgeCount);

    if (begin >= end)
      continue;

    jobs.push_back(std::async(
        std::launch::async,
        [this, contextCopy, edges, shadowDir, sunHeight, alpha, begin, end]()
        {
          ShadowBuildResult result;

          for (std::size_t i = begin; i < end; i++)
          {
            const TerrainShadowEdge& edge = (*edges)[i];

            if (edge.topElevation - edge.bottomElevation <= 0)
              continue;

            if (!shouldCastTerrainShadow(edge, shadowDir))
              continue;

            constructTerrainEdgeShadowProjectedClipped(
                *contextCopy,
                result.items,
                edge,
                shadowDir,
                sunHeight,
                m_shadowSettings.terrainShadowMaxLength,
                alpha);

            result.edgesProcessed++;
          }

          return result;
        }));
  }

  return jobs;
}
#endif

} // namespace sfs
