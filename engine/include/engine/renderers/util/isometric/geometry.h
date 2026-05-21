#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include "glm/glm/geometric.hpp"
#include <vector>

namespace sfs
{

inline static float cross2D(const glm::vec2& a, const glm::vec2& b)
{
  return a.x * b.y - a.y * b.x;
}

inline static std::vector<glm::vec2>
clipPolygonAgainstEdge(const std::vector<glm::vec2>& input,
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

inline static std::vector<glm::vec2>
clipPolygonToTile(const glm::vec2 worldPoints[4], const glm::ivec2& tile)
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

} // namespace sfs
