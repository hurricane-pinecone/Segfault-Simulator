#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include "glm/glm/geometric.hpp"
#include <cstddef>

namespace sfs
{

struct ClippedPolygon
{
  glm::vec2 points[8];
  int count = 0;
};

struct TileBounds
{
  glm::ivec2 min{0, 0};
  glm::ivec2 max{0, 0};
  bool valid = false;
};

struct TerrainShadowEdge
{
  enum class Side
  {
    West,
    East,
    North,
    South,
  };
  glm::vec2 a;
  glm::vec2 b;

  glm::ivec2 casterTile{0, 0};
  glm::ivec2 receiverTile{0, 0};

  int topElevation = 0;
  int bottomElevation = 0;

  Side side = Side::West;
};

struct TerrainElevationGridView
{
  const int* elevations = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;
  glm::ivec2 origin{0, 0};

  bool valid() const
  {
    return elevations != nullptr && width > 0 && height > 0 && stride >= width;
  }

  bool tryGet(const glm::ivec2& tile, int& elevation) const
  {
    const int x = tile.x - origin.x;
    const int y = tile.y - origin.y;

    if (x < 0 || y < 0 || x >= width || y >= height)
      return false;

    elevation = elevations[y * stride + x];
    return true;
  }
};

inline static float cross2D(const glm::vec2& a, const glm::vec2& b)
{
  return a.x * b.y - a.y * b.x;
}

inline static void clipPolygonAgainstEdge(const glm::vec2* input,
                                          int inputCount,
                                          glm::vec2* output,
                                          int& outputCount,
                                          float value,
                                          int axis,
                                          bool keepGreater)
{
  outputCount = 0;

  if (inputCount <= 0)
    return;

  auto inside = [&](const glm::vec2& p)
  { return keepGreater ? p[axis] >= value : p[axis] <= value; };

  auto intersect = [&](const glm::vec2& a, const glm::vec2& b)
  {
    const float da = a[axis] - value;
    const float db = b[axis] - value;
    const float t = da / (da - db);

    return a + (b - a) * t;
  };

  glm::vec2 previous = input[inputCount - 1];
  bool previousInside = inside(previous);

  for (int i = 0; i < inputCount; i++)
  {
    const glm::vec2 current = input[i];
    const bool currentInside = inside(current);

    if (currentInside != previousInside)
      output[outputCount++] = intersect(previous, current);

    if (currentInside)
      output[outputCount++] = current;

    previous = current;
    previousInside = currentInside;
  }
}

inline static ClippedPolygon clipPolygonToTile(const glm::vec2 worldPoints[4],
                                               const glm::ivec2& tile)
{
  glm::vec2 bufferA[8] = {
      worldPoints[0],
      worldPoints[1],
      worldPoints[2],
      worldPoints[3],
  };

  glm::vec2 bufferB[8];

  int countA = 4;
  int countB = 0;

  const float minX = static_cast<float>(tile.x);
  const float maxX = static_cast<float>(tile.x + 1);
  const float minY = static_cast<float>(tile.y);
  const float maxY = static_cast<float>(tile.y + 1);

  clipPolygonAgainstEdge(bufferA, countA, bufferB, countB, minX, 0, true);
  clipPolygonAgainstEdge(bufferB, countB, bufferA, countA, maxX, 0, false);
  clipPolygonAgainstEdge(bufferA, countA, bufferB, countB, minY, 1, true);
  clipPolygonAgainstEdge(bufferB, countB, bufferA, countA, maxY, 1, false);

  ClippedPolygon result;
  result.count = countA;

  for (int i = 0; i < countA; i++)
    result.points[i] = bufferA[i];

  return result;
}

inline static bool pointInConvexQuad(glm::vec2 p, const glm::vec2 quad[4])
{
  bool hasPositive = false;
  bool hasNegative = false;

  for (int i = 0; i < 4; i++)
  {
    const glm::vec2 a = quad[i];
    const glm::vec2 b = quad[(i + 1) % 4];

    const float c = cross2D(b - a, p - a);

    if (c > 0.0001f)
      hasPositive = true;

    if (c < -0.0001f)
      hasNegative = true;
  }

  return !(hasPositive && hasNegative);
}

inline static bool
segmentIntersectsSegment(glm::vec2 p1, glm::vec2 p2, glm::vec2 q1, glm::vec2 q2)
{
  const glm::vec2 r = p2 - p1;
  const glm::vec2 s = q2 - q1;

  const float denom = cross2D(r, s);

  if (std::abs(denom) < 0.0001f)
    return false;

  const float t = cross2D(q1 - p1, s) / denom;
  const float u = cross2D(q1 - p1, r) / denom;

  return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
}

inline static bool projectShadowOntoWallEdge(const glm::vec2 shadowPoly[4],
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

inline static void
getTileWallEdge(const glm::ivec2& tile, int side, glm::vec2& a, glm::vec2& b)
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

template <typename Visitor>
void forEachTileOverlappingShadowQuad(const glm::vec2 points[4],
                                      const glm::vec2& shadowDir,
                                      Visitor&& visit)
{
  float minX = points[0].x;
  float maxX = points[0].x;
  float minY = points[0].y;
  float maxY = points[0].y;

  for (int i = 1; i < 4; i++)
  {
    minX = std::min(minX, points[i].x);
    maxX = std::max(maxX, points[i].x);
    minY = std::min(minY, points[i].y);
    maxY = std::max(maxY, points[i].y);
  }

  const int tileMinX = static_cast<int>(std::floor(minX));
  const int tileMaxX = static_cast<int>(std::floor(maxX));
  const int tileMinY = static_cast<int>(std::floor(minY));
  const int tileMaxY = static_cast<int>(std::floor(maxY));

  const int xStart = shadowDir.x >= 0.0f ? tileMinX : tileMaxX;
  const int xEnd = shadowDir.x >= 0.0f ? tileMaxX : tileMinX;
  const int xStep = shadowDir.x >= 0.0f ? 1 : -1;

  const int yStart = shadowDir.y >= 0.0f ? tileMinY : tileMaxY;
  const int yEnd = shadowDir.y >= 0.0f ? tileMaxY : tileMinY;
  const int yStep = shadowDir.y >= 0.0f ? 1 : -1;

  for (int y = yStart; y != yEnd + yStep; y += yStep)
  {
    for (int x = xStart; x != xEnd + xStep; x += xStep)
    {
      const glm::ivec2 tile{x, y};

      const ClippedPolygon clipped = clipPolygonToTile(points, tile);

      if (clipped.count < 3)
        continue;

      if (!visit(tile, clipped))
        return;
    }
  }
}

inline static void expandTileBounds(TileBounds& bounds, const glm::ivec2& tile)
{
  if (!bounds.valid)
  {
    bounds.min = tile;
    bounds.max = tile;
    bounds.valid = true;
    return;
  }

  bounds.min = glm::min(bounds.min, tile);
  bounds.max = glm::max(bounds.max, tile);
}

inline static TileBounds expandedTileBounds(TileBounds bounds, int padding)
{
  if (!bounds.valid)
    return bounds;

  bounds.min -= glm::ivec2{padding, padding};
  bounds.max += glm::ivec2{padding, padding};
  return bounds;
}

inline static bool shadowQuadOverlapsTileBounds(const glm::vec2 points[4],
                                                const TileBounds& bounds)
{
  if (!bounds.valid)
    return false;

  float minX = points[0].x;
  float maxX = points[0].x;
  float minY = points[0].y;
  float maxY = points[0].y;

  for (int i = 1; i < 4; i++)
  {
    minX = std::min(minX, points[i].x);
    maxX = std::max(maxX, points[i].x);
    minY = std::min(minY, points[i].y);
    maxY = std::max(maxY, points[i].y);
  }

  return maxX >= static_cast<float>(bounds.min.x) &&
         minX <= static_cast<float>(bounds.max.x + 1) &&
         maxY >= static_cast<float>(bounds.min.y) &&
         minY <= static_cast<float>(bounds.max.y + 1);
}

inline static TileBounds
getTerrainShadowEdgeTileBounds(const std::vector<TerrainShadowEdge>& edges)
{
  TileBounds bounds;

  for (const TerrainShadowEdge& edge : edges)
  {
    expandTileBounds(bounds, edge.casterTile);
    expandTileBounds(bounds, edge.receiverTile);
  }

  return bounds;
}

inline static glm::vec2 isometricTogrid(const glm::vec2& iso,
                                        int tileWidth,
                                        int tileHeight,
                                        float worldScale = 1.0f)
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

inline glm::vec2 gridToIsometric(const glm::vec2& gridPosition,
                                 int tileWidth,
                                 int tileHeight,
                                 float worldScale)
{
  return {
      (gridPosition.x - gridPosition.y) *
          (static_cast<float>(tileWidth) * worldScale) * 0.5f,

      (gridPosition.x + gridPosition.y) *
          (static_cast<float>(tileHeight) * worldScale) * 0.5f,
  };
}

inline static glm::ivec2 gridCellOf(const glm::vec2& position)
{
  return {
      static_cast<int>(std::floor(position.x)),
      static_cast<int>(std::floor(position.y)),
  };
}

} // namespace sfs
