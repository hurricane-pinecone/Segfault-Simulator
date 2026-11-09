#include "engine/core/rendering/debugLines.h"

#include "glm/glm/common.hpp"
#include "glm/glm/ext/matrix_float3x3.hpp"
#include "glm/glm/ext/scalar_constants.hpp"
#include "glm/glm/ext/vector_float2.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace sfs
{

namespace
{
glm::vec3 place(const glm::mat4& placed, const glm::vec3& point)
{
  return glm::vec3(placed * glm::vec4(point, 1.0f));
}
} // namespace

uint32_t DebugLines::pack(const glm::vec4& color)
{
  const glm::vec4 c = glm::clamp(color, 0.0f, 1.0f) * 255.0f + 0.5f;
  return static_cast<uint32_t>(c.r) | (static_cast<uint32_t>(c.g) << 8) |
         (static_cast<uint32_t>(c.b) << 16) |
         (static_cast<uint32_t>(c.a) << 24);
}

void DebugLines::line(const glm::vec3& from,
                      const glm::vec3& to,
                      uint32_t color)
{
  m_vertices.push_back({from, color});
  m_vertices.push_back({to, color});
}

void DebugLines::box(const glm::mat4& placed,
                     const glm::vec3& centre,
                     const glm::vec3& half,
                     uint32_t color)
{
  glm::vec3 corners[8];
  for (int i = 0; i < 8; ++i)
    corners[i] = place(placed,
                       centre + glm::vec3((i & 1) ? half.x : -half.x,
                                          (i & 2) ? half.y : -half.y,
                                          (i & 4) ? half.z : -half.z));
  for (int a = 0; a < 8; ++a)
    for (const int bit : {1, 2, 4})
      if (!(a & bit))
        line(corners[a], corners[a | bit], color);
}

void DebugLines::circle(const glm::mat4& placed,
                        const glm::vec3& centre,
                        const glm::vec3& u,
                        const glm::vec3& v,
                        float radius,
                        uint32_t color,
                        float start,
                        float end)
{
  const int segments =
      std::max(4, static_cast<int>(std::ceil(kCircleSegments * (end - start))));
  const float tau = 2.0f * glm::pi<float>();
  // Placed once: the centre, and the radius along each axis, in the world.
  const glm::vec3 middle = place(placed, centre);
  const glm::mat3 turn(placed);
  const glm::vec3 across = turn * (u * radius);
  const glm::vec3 up = turn * (v * radius);
  // Whole numbers of a full circle's segments (every arc the shapes draw)
  // from a table: no sine or cosine a point.
  static const std::array<glm::vec2, kCircleSegments> unit = []
  {
    std::array<glm::vec2, kCircleSegments> points{};
    for (int k = 0; k < kCircleSegments; ++k)
    {
      const float angle = 2.0f * glm::pi<float>() * k / kCircleSegments;
      points[k] = {std::cos(angle), std::sin(angle)};
    }
    return points;
  }();
  const float first = start * kCircleSegments;
  const int from = static_cast<int>(std::lround(first));
  const bool tabled =
      std::abs(first - from) < 1e-4f &&
      std::abs((end - start) * kCircleSegments - segments) < 1e-4f;
  const auto at = [&](int i)
  {
    if (tabled)
    {
      const int k =
          ((from + i) % kCircleSegments + kCircleSegments) % kCircleSegments;
      return middle + unit[k].x * across + unit[k].y * up;
    }
    const float angle = tau * (start + (end - start) * i / segments);
    return middle + std::cos(angle) * across + std::sin(angle) * up;
  };
  glm::vec3 previous = at(0);
  for (int i = 1; i <= segments; ++i)
  {
    const glm::vec3 next = at(i);
    line(previous, next, color);
    previous = next;
  }
}

void DebugLines::sphere(const glm::mat4& placed,
                        const glm::vec3& centre,
                        float radius,
                        uint32_t color)
{
  const glm::vec3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
  circle(placed, centre, x, y, radius, color);
  circle(placed, centre, x, z, radius, color);
  circle(placed, centre, y, z, radius, color);
}

void DebugLines::capsule(const glm::mat4& placed,
                         const glm::vec3& centre,
                         float radius,
                         float halfLength,
                         uint32_t color)
{
  const glm::vec3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
  const glm::vec3 top = centre + z * halfLength;
  const glm::vec3 bottom = centre - z * halfLength;
  circle(placed, top, x, y, radius, color);
  circle(placed, bottom, x, y, radius, color);
  for (const glm::vec3& side : {x, -x, y, -y})
    line(place(placed, top + side * radius),
         place(placed, bottom + side * radius),
         color);
  // The rounded ends: half circles over the top and under the bottom.
  circle(placed, top, x, z, radius, color, 0.0f, 0.5f);
  circle(placed, top, y, z, radius, color, 0.0f, 0.5f);
  circle(placed, bottom, x, z, radius, color, 0.5f, 1.0f);
  circle(placed, bottom, y, z, radius, color, 0.5f, 1.0f);
}

} // namespace sfs
