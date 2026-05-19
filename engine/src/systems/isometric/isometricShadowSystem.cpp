#include "engine/systems/isometric/isometricShadowSystem.h"
#include "engine/utils/algorithms/gridDDA.h"

#include <algorithm>
#include <unordered_set>
#ifdef __EMSCRIPTEN__
  #include <chrono>
#endif
#include <cmath>
#include <future>
#include <thread>

#include "engine/components/elevationComponent.h"
#include "engine/components/tags/isometricTile.h"
#include "engine/components/terrainBoundaryComponent.h"
#include "engine/renderers/isometricRenderItem.h"
#include "engine/renderers/isometricRenderQueue.h"
#include "engine/systems/isometric/isometricRenderSystem.h"
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

void IsometricShadowSystem::submitTerrainShadows(
    const IsometricRenderContext& context,
    const IsometricAmbientLighting& ambient,
    IsometricRenderQueue& queue)
{
  if (m_cache.edgesDirty || m_cache.edges.empty())
  {
    m_cache.edges = mergeTerrainShadowEdges(getTerrainShadowEdges(context));

    m_cache.edgesDirty = false;
    m_cache.itemsDirty = true;
  }

  const glm::vec3 sunDir3D = ambient.direction;

  if (sunDir3D.z <= 0.02f)
    return;

  glm::vec2 shadowDir{-sunDir3D.x, -sunDir3D.y};

  const float horizontalAmount = glm::length(shadowDir);

  if (horizontalAmount <= 0.001f)
  {
    m_cache.items.clear();
    m_cache.itemsDirty = true;
    m_cache.sunDir = sunDir3D;
    return;
  }

  shadowDir /= horizontalAmount;

  const float sunHeight = std::max(sunDir3D.z, 0.08f);
  const float effectiveSunHeight =
      sunHeight / std::max(horizontalAmount, 0.001f);

  const float diffuse = std::clamp(ambient.diffuseStrength, 0.0f, 1.0f);

  if (diffuse <= 0.01f)
  {
    m_cache.items.clear();
    m_cache.itemsDirty = true;
    return;
  }

  const float alpha = m_shadowSettings.terrainShadowAlpha * diffuse;

  if (alpha < 0.04f)
  {
    m_cache.items.clear();
    m_cache.itemsDirty = true;
    return;
  }

  const bool sunChanged = glm::length(sunDir3D - m_cache.sunDir) > 0.025f;

  const bool cameraChanged = glm::length(context.isoCameraPosition -
                                         m_cache.isoCameraPosition) > 0.001f ||
                             std::abs(context.zoom - m_cache.zoom) > 0.001f;

  if (!m_cache.itemsDirty && !sunChanged && !cameraChanged)
  {
    queue.submitAll(m_cache.items);
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
      std::vector<IsometricRenderItem> newItems;

      for (auto& job : m_shadowJobs)
      {
        ShadowBuildResult result = job.get();

        gTerrainShadowEdgesProcessed += result.edgesProcessed;

        newItems.insert(
            newItems.end(), result.items.begin(), result.items.end());
      }

      m_cache.items = std::move(newItems);
      m_cache.sunDir = sunDir3D;
      m_cache.isoCameraPosition = context.isoCameraPosition;
      m_cache.zoom = context.zoom;
      m_cache.itemsDirty = false;

      m_shadowJobs.clear();
      m_shadowBuildInProgress = false;
    }

    queue.submitAll(m_cache.items);
    return;
  }

  m_shadowJobs =
      startTerrainEdgeShadowJobs(context, shadowDir, effectiveSunHeight, alpha);

  m_shadowBuildInProgress = !m_shadowJobs.empty();

  queue.submitAll(m_cache.items);
  return;
#else
  m_cache.items = buildTerrainEdgeShadowItems(
      context, shadowDir, effectiveSunHeight, alpha);
#endif

  queue.submitAll(m_cache.items);

  m_cache.sunDir = sunDir3D;
  m_cache.isoCameraPosition = context.isoCameraPosition;
  m_cache.zoom = context.zoom;
  m_cache.itemsDirty = false;
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

void IsometricShadowSystem::constructTerrainShadow(
    std::vector<IsometricRenderItem>& outItems,
    const glm::vec2 screenPoints[4],
    SDL_Color tint,
    float sortKey)
{
  IsometricRenderItem item;

  item.isShadow = true;
  item.isTerrainShadow = true;
  item.tint = tint;
  item.sortKey = sortKey;
  item.renderLayer = 1;

  for (int i = 0; i < 4; i++)
    item.shadowScreenPoints[i] = screenPoints[i];

  outItems.push_back(item);
}

void IsometricShadowSystem::constructTileShadowPolygonAt(
    const IsometricRenderContext& context,
    std::vector<IsometricRenderItem>& outItems,
    const glm::ivec2& tile,
    int elevation,
    const glm::vec2 worldPoints[4],
    float alpha)
{
  const std::vector<glm::vec2> clipped = clipPolygonToTile(worldPoints, tile);

  if (clipped.size() < 3)
    return;

  constexpr float ElevationSortWeight = 0.5f;

  const float sortKey =
      static_cast<float>(tile.x) + static_cast<float>(tile.y) +
      static_cast<float>(elevation) * ElevationSortWeight + 0.0005f;

  for (size_t i = 1; i + 1 < clipped.size(); i++)
  {
    glm::vec2 screenPoints[4] = {
        context.worldToScreen(clipped[0], static_cast<float>(elevation)),
        context.worldToScreen(clipped[i], static_cast<float>(elevation)),
        context.worldToScreen(clipped[i + 1], static_cast<float>(elevation)),
        context.worldToScreen(clipped[i + 1], static_cast<float>(elevation)),
    };

    constructTerrainShadow(
        outItems,
        screenPoints,
        SDL_Color{0, 0, 0, static_cast<Uint8>(alpha * 255.0f)},
        sortKey);
  }
}

void IsometricShadowSystem::constructTerrainEdgeShadowProjectedClipped(
    const IsometricRenderContext& context,
    std::vector<IsometricRenderItem>& outItems,
    const TerrainShadowEdge& edge,
    const glm::vec2& shadowDir,
    float sunHeight,
    float maxShadowLength,
    float alpha)
{
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

  std::unordered_set<glm::ivec2, IVec2Hash> visited;

  auto visitShadowTile = [&](const glm::ivec2& tile, bool skipPathCheck)
  {
    if (!visited.insert(tile).second)
      return true;

    if (!skipPathCheck &&
        terrainShadowPathBlocked(context, edge, tile, shadowDir))
    {
      return true;
    }

    tryConstructShadowOnTile(context,
                             outItems,
                             tile,
                             edge.bottomElevation,
                             shadowWorldPoints,
                             alpha);

    return true;
  };

  constructSeedShadowStrip(edge, visitShadowTile);

  walkShadowEdgeRays(edge, shadowDir, shadowLength, visitShadowTile);
}

bool IsometricShadowSystem::terrainShadowPathBlocked(
    const IsometricRenderContext& context,
    const TerrainShadowEdge& edge,
    const glm::ivec2& receiverTile,
    const glm::vec2& shadowDir) const
{

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

std::vector<glm::vec2> IsometricShadowSystem::clipPolygonAgainstEdge(
    const std::vector<glm::vec2>& input,
    float edge,
    int axis,
    bool keepGreater)
{
  std::vector<glm::vec2> output;

  if (input.empty())
    return output;

  auto inside = [&](const glm::vec2& p)
  {
    const float value = axis == 0 ? p.x : p.y;
    return keepGreater ? value >= edge : value <= edge;
  };

  auto intersect = [&](const glm::vec2& a, const glm::vec2& b)
  {
    const float av = axis == 0 ? a.x : a.y;
    const float bv = axis == 0 ? b.x : b.y;
    const float t = (edge - av) / (bv - av);

    return a + (b - a) * t;
  };

  glm::vec2 previous = input.back();
  bool previousInside = inside(previous);

  for (const glm::vec2& current : input)
  {
    const bool currentInside = inside(current);

    if (currentInside)
    {
      if (!previousInside)
        output.push_back(intersect(previous, current));

      output.push_back(current);
    }
    else if (previousInside)
    {
      output.push_back(intersect(previous, current));
    }

    previous = current;
    previousInside = currentInside;
  }

  return output;
}

std::vector<glm::vec2>
IsometricShadowSystem::clipPolygonToTile(const glm::vec2 worldPoints[4],
                                         const glm::ivec2& tile)
{
  std::vector<glm::vec2> polygon = {
      worldPoints[0],
      worldPoints[1],
      worldPoints[2],
      worldPoints[3],
  };

  const float minX = static_cast<float>(tile.x);
  const float maxX = static_cast<float>(tile.x + 1);
  const float minY = static_cast<float>(tile.y);
  const float maxY = static_cast<float>(tile.y + 1);

  polygon = clipPolygonAgainstEdge(polygon, minX, 0, true);
  polygon = clipPolygonAgainstEdge(polygon, maxX, 0, false);
  polygon = clipPolygonAgainstEdge(polygon, minY, 1, true);
  polygon = clipPolygonAgainstEdge(polygon, maxY, 1, false);

  return polygon;
}

void IsometricShadowSystem::constructWallShadowFace(
    const IsometricRenderContext& renderContext,
    std::vector<IsometricRenderItem>& outItems,
    const glm::ivec2& tile,
    int elevation,
    int side,
    const glm::vec2 shadowWorldPoints[4],
    float incomingElevation,
    float normalizedDistance,
    float alpha)
{
  glm::vec2 a;
  glm::vec2 b;
  getWallEdge(tile, side, a, b);

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

  constructTerrainShadow(outItems,
                         screenPoints,
                         SDL_Color{0, 0, 0, static_cast<Uint8>(alpha * 255.0f)},
                         sortKey);
}

void IsometricShadowSystem::getWallEdge(const glm::ivec2& tile,
                                        int side,
                                        glm::vec2& a,
                                        glm::vec2& b)
{
  if (side == 0)
  {
    a = glm::vec2{tile.x, tile.y};
    b = glm::vec2{tile.x, tile.y + 1};
  }
  else if (side == 1)
  {
    a = glm::vec2{tile.x, tile.y};
    b = glm::vec2{tile.x + 1, tile.y};
  }
  else if (side == 2)
  {
    a = glm::vec2{tile.x + 1, tile.y};
    b = glm::vec2{tile.x + 1, tile.y + 1};
  }
  else
  {
    a = glm::vec2{tile.x, tile.y + 1};
    b = glm::vec2{tile.x + 1, tile.y + 1};
  }
}

bool IsometricShadowSystem::segmentIntersectsSegment(glm::vec2 p1,
                                                     glm::vec2 p2,
                                                     glm::vec2 q1,
                                                     glm::vec2 q2)
{
  auto cross = [](glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; };

  const glm::vec2 r = p2 - p1;
  const glm::vec2 s = q2 - q1;

  const float denom = cross(r, s);

  if (std::abs(denom) < 0.0001f)
    return false;

  const float t = cross(q1 - p1, s) / denom;
  const float u = cross(q1 - p1, r) / denom;

  return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
}

bool IsometricShadowSystem::pointInConvexQuad(glm::vec2 p,
                                              const glm::vec2 quad[4])
{
  auto cross = [](glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; };

  bool hasPositive = false;
  bool hasNegative = false;

  for (int i = 0; i < 4; i++)
  {
    const glm::vec2 a = quad[i];
    const glm::vec2 b = quad[(i + 1) % 4];

    const float c = cross(b - a, p - a);

    if (c > 0.0001f)
      hasPositive = true;

    if (c < -0.0001f)
      hasNegative = true;
  }

  return !(hasPositive && hasNegative);
}

bool IsometricShadowSystem::projectShadowOntoWallEdge(
    const glm::vec2 shadowPoly[4],
    glm::vec2 wallA,
    glm::vec2 wallB,
    float& outMinT,
    float& outMaxT)
{
  outMinT = 1.0f;
  outMaxT = 0.0f;

  const glm::vec2 wall = wallB - wallA;
  const float wallLen2 = glm::dot(wall, wall);

  if (wallLen2 < 0.0001f)
    return false;

  auto addT = [&](float t)
  {
    outMinT = std::min(outMinT, t);
    outMaxT = std::max(outMaxT, t);
  };

  constexpr int Samples = 12;

  for (int i = 0; i <= Samples; i++)
  {
    const float t = static_cast<float>(i) / static_cast<float>(Samples);
    const glm::vec2 p = wallA + wall * t;

    if (pointInConvexQuad(p, shadowPoly))
      addT(t);
  }

  return outMaxT > outMinT;
}

std::vector<TerrainShadowEdge> IsometricShadowSystem::mergeTerrainShadowEdges(
    const std::vector<TerrainShadowEdge>& input) const
{
  struct Run
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

std::vector<IsometricRenderItem>
IsometricShadowSystem::buildTerrainEdgeShadowItems(
    const IsometricRenderContext& context,
    const glm::vec2& shadowDir,
    float sunHeight,
    float alpha)
{
  const std::vector<TerrainShadowEdge> edges = m_cache.edges;
  const std::size_t edgeCount = edges.size();

  struct ShadowBuildResult
  {
    std::vector<IsometricRenderItem> items;
    int edgesProcessed = 0;
  };

  if (edgeCount == 0)
    return {};

#ifdef __EMSCRIPTEN__
  const unsigned int hardwareThreads = 2;
#else
  const unsigned int hardwareThreads =
      std::max(1u, std::thread::hardware_concurrency());
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

  std::vector<IsometricRenderItem> items;

  for (auto& job : jobs)
  {
    ShadowBuildResult result = job.get();

    gTerrainShadowEdgesProcessed += result.edgesProcessed;

    items.insert(items.end(), result.items.begin(), result.items.end());
  }

  return items;
}

void IsometricShadowSystem::submitSpriteShadow(
    const IsometricRenderContext& context,
    const IsometricRenderItem& caster,
    const glm::vec2& casterWorldSample,
    int casterElevation,
    const IsometricAmbientLighting* ambientLighting,
    const std::vector<IsometricPointLightSnapshot>* pointLights,
    IsometricRenderQueue& queue)
{
  if (!ambientLighting && (!pointLights || pointLights->empty()))
    return;

  const float sunStrength =
      ambientLighting ? std::clamp(ambientLighting->diffuseStrength, 0.0f, 1.0f)
                      : 0.0f;

  const float pointShadowStrength = 1.0f - sunStrength;

  std::vector<IsometricRenderItem> items;

  auto submitTexturedSpriteShadow =
      [&](const glm::vec2& shadowDir, float shadowLength, float alpha)
  {
    if (alpha <= 0.01f || shadowLength <= 0.01f)
      return;

    glm::vec2 dir = shadowDir;

    if (glm::length(dir) > 0.001f)
      dir = glm::normalize(dir);
    else
      return;

    const glm::vec2 side{-dir.y, dir.x};

    constexpr float SpriteShadowBaseWidth = 1.0f;
    constexpr float SpriteShadowTipWidth = 1.0f;

    const glm::vec2 base = casterWorldSample;
    const glm::vec2 tip = base + dir * shadowLength;

    glm::vec2 groundPoints[4] = {
        base - side * SpriteShadowBaseWidth * 0.5f,
        base + side * SpriteShadowBaseWidth * 0.5f,
        tip + side * SpriteShadowTipWidth * 0.5f,
        tip - side * SpriteShadowTipWidth * 0.5f,
    };

    IsometricRenderItem shadow = caster;

    shadow.isShadow = true;
    shadow.isTerrainShadow = false;
    shadow.hasNormalMap = false;
    shadow.renderLayer = 1;
    const float tipSortKey =
        tip.x + tip.y + static_cast<float>(casterElevation) * 0.5f;

    shadow.sortKey = tipSortKey + 0.0004f;
    shadow.renderLayer = 1;

    shadow.tint = SDL_Color{
        0,
        0,
        0,
        static_cast<Uint8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f),
    };

    for (int i = 0; i < 4; i++)
    {
      shadow.shadowScreenPoints[i] = context.worldToScreen(
          groundPoints[i], static_cast<float>(casterElevation));
    }

    items.push_back(shadow);
  };

  //
  // Sun shadows
  //
  if (ambientLighting)
  {
    const glm::vec3 sunDir3D = ambientLighting->direction;

    const float diffuse =
        std::clamp(ambientLighting->diffuseStrength, 0.0f, 1.0f);

    if (sunDir3D.z > 0.02f && diffuse > 0.01f)
    {
      glm::vec2 shadowDir{-sunDir3D.x, -sunDir3D.y};

      const float horizontalAmount = glm::length(shadowDir);

      if (horizontalAmount > 0.001f)
      {
        shadowDir /= horizontalAmount;

        const float sunHeight = std::max(sunDir3D.z, 0.08f);

        constexpr float SpriteCasterHeight = 0.75f;

        const float shadowLength =
            std::min(m_shadowSettings.spriteShadowMaxLength,
                     SpriteCasterHeight * horizontalAmount / sunHeight);

        const float alpha = m_shadowSettings.spriteShadowAlpha * diffuse;

        submitTexturedSpriteShadow(shadowDir, shadowLength, alpha);
      }
    }
  }

  //
  // Point-light shadows
  //
  if (pointLights)
  {
    for (const auto& light : *pointLights)
    {
      const glm::vec2 toCaster = casterWorldSample - light.worldPosition;
      const float distance = glm::length(toCaster);

      if (distance <= 0.001f || distance >= light.radius)
        continue;

      float attenuation = 1.0f - distance / light.radius;
      attenuation = std::clamp(attenuation, 0.0f, 1.0f);
      attenuation *= attenuation;

      const float lightFactor =
          std::clamp(light.intensity * attenuation, 0.0f, 1.0f);

      const float alpha = std::clamp(m_shadowSettings.spriteShadowAlpha *
                                         lightFactor * pointShadowStrength,
                                     0.0f,
                                     1.0f);

      if (alpha <= 0.01f)
        continue;

      constexpr float SpriteCasterHeightWorld = 1.0f;

      const float elevationStepPixels =
          static_cast<float>(context.elevationStep) * context.worldScale;

      const float lightHeightWorld =
          std::max(light.height / elevationStepPixels, 0.08f);

      float shadowLength =
          SpriteCasterHeightWorld * distance / lightHeightWorld;

      shadowLength = std::clamp(
          shadowLength, 0.15f, m_shadowSettings.spriteShadowMaxLength);

      submitTexturedSpriteShadow(toCaster, shadowLength, alpha);
    }
  }

  queue.submitAll(items);
}

void IsometricShadowSystem::setTerrainShadowMaxLength(float length)
{
  m_shadowSettings.terrainShadowMaxLength = std::max(length, 0.0f);

  markTerrainDirty();
}

void IsometricShadowSystem::setSpriteShadowMaxLength(float length)
{
  m_shadowSettings.spriteShadowMaxLength = std::max(length, 0.0f);
}

void IsometricShadowSystem::setTerrainShadowAlpha(float alpha)
{
  m_shadowSettings.terrainShadowAlpha = std::clamp(alpha, 0.0f, 1.0f);

  markTerrainDirty();
}

void IsometricShadowSystem::setSpriteShadowAlpha(float alpha)
{
  m_shadowSettings.spriteShadowAlpha = std::clamp(alpha, 0.0f, 1.0f);
}

bool IsometricShadowSystem::tryConstructShadowOnTile(
    const IsometricRenderContext& context,
    std::vector<IsometricRenderItem>& outItems,
    const glm::ivec2& tile,
    int requiredElevation,
    const glm::vec2 worldPoints[4],
    float alpha)
{
  if (!context.hasTileAt(tile))
    return false;

  const int receiverElevation =
      context.getTileElevationAt(glm::vec2{tile.x, tile.y});

  if (receiverElevation != requiredElevation)
    return false;

  constructTileShadowPolygonAt(
      context, outItems, tile, receiverElevation, worldPoints, alpha);

  return true;
}

float IsometricShadowSystem::calculateTerrainShadowLength(
    const TerrainShadowEdge& edge,
    float sunHeight,
    float maxShadowLength)
{
  const int heightDelta = edge.topElevation - edge.bottomElevation;

  if (heightDelta <= 0)
    return 0.0f;

  constexpr float TerrainShadowLengthScale = 1.35f;

  return std::min(
      maxShadowLength,
      static_cast<float>(heightDelta) * TerrainShadowLengthScale / sunHeight);
}

#ifdef __EMSCRIPTEN__
std::vector<std::future<IsometricShadowSystem::ShadowBuildResult>>
IsometricShadowSystem::startTerrainEdgeShadowJobs(
    const IsometricRenderContext& context,
    const glm::vec2& shadowDir,
    float sunHeight,
    float alpha)
{
  const std::vector<TerrainShadowEdge> edges = m_cache.edges;
  const std::size_t edgeCount = edges.size();

  constexpr float TerrainShadowMaxLength = 3.0f;
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
        [this, context, edges, shadowDir, sunHeight, alpha, begin, end]()
        {
          ShadowBuildResult result;

          for (std::size_t i = begin; i < end; i++)
          {
            const TerrainShadowEdge& edge = edges[i];

            if (edge.topElevation - edge.bottomElevation <= 0)
              continue;

            if (!shouldCastTerrainShadow(edge, shadowDir))
              continue;

            submitTerrainEdgeShadowProjectedClipped(context,
                                                    result.items,
                                                    edge,
                                                    shadowDir,
                                                    sunHeight,
                                                    TerrainShadowMaxLength,
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
