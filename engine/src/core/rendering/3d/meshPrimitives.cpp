#include "engine/core/rendering/3d/meshPrimitives.h"

#include "glm/glm/ext/scalar_constants.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/matrix.hpp"
#include "glm/glm/trigonometric.hpp"

#include <algorithm>
#include <cmath>

namespace sfs::meshes
{

namespace
{

constexpr float kTwoPi = 2.0f * glm::pi<float>();

// Append a flat quad (a, b, c, d counter-clockwise seen from the front).
void addQuad(MeshData& mesh,
             const glm::vec3& a,
             const glm::vec3& b,
             const glm::vec3& c,
             const glm::vec3& d,
             const glm::vec3& color)
{
  const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
  const auto base = static_cast<uint32_t>(mesh.vertices.size());

  for (const glm::vec3& p : {a, b, c, d})
    mesh.vertices.push_back({p, normal, color});

  mesh.indices.insert(mesh.indices.end(),
                      {base, base + 1, base + 2, base, base + 2, base + 3});
}

// Append a flat triangle (counter-clockwise seen from the front).
void addTriangle(MeshData& mesh,
                 const glm::vec3& a,
                 const glm::vec3& b,
                 const glm::vec3& c,
                 const glm::vec3& color)
{
  const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
  const auto base = static_cast<uint32_t>(mesh.vertices.size());

  for (const glm::vec3& p : {a, b, c})
    mesh.vertices.push_back({p, normal, color});

  mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
}

// Append a flat disc cap at height z, facing up (+Z) or down (-Z).
void addCap(MeshData& mesh,
            float radius,
            float z,
            int segments,
            bool facingUp,
            const glm::vec3& color)
{
  const glm::vec3 normal{0.0f, 0.0f, facingUp ? 1.0f : -1.0f};
  const auto center = static_cast<uint32_t>(mesh.vertices.size());
  mesh.vertices.push_back({{0.0f, 0.0f, z}, normal, color});

  for (int i = 0; i < segments; ++i)
  {
    const float angle = kTwoPi * static_cast<float>(i) / segments;
    mesh.vertices.push_back(
        {{radius * std::cos(angle), radius * std::sin(angle), z},
         normal,
         color});
  }

  for (int i = 0; i < segments; ++i)
  {
    const uint32_t current = center + 1 + static_cast<uint32_t>(i);
    const uint32_t next =
        center + 1 + static_cast<uint32_t>((i + 1) % segments);

    if (facingUp)
      mesh.indices.insert(mesh.indices.end(), {center, current, next});
    else
      mesh.indices.insert(mesh.indices.end(), {center, next, current});
  }
}

} // namespace

MeshData box(const glm::vec3& size, const glm::vec3& color)
{
  const float x = size.x * 0.5f;
  const float y = size.y * 0.5f;
  const float z = size.z;

  MeshData mesh;
  mesh.vertices.reserve(24);
  mesh.indices.reserve(36);

  // +Z top, -Z bottom
  addQuad(mesh, {-x, -y, z}, {x, -y, z}, {x, y, z}, {-x, y, z}, color);
  addQuad(mesh, {-x, y, 0}, {x, y, 0}, {x, -y, 0}, {-x, -y, 0}, color);
  // +X, -X
  addQuad(mesh, {x, -y, 0}, {x, y, 0}, {x, y, z}, {x, -y, z}, color);
  addQuad(mesh, {-x, y, 0}, {-x, -y, 0}, {-x, -y, z}, {-x, y, z}, color);
  // +Y, -Y
  addQuad(mesh, {x, y, 0}, {-x, y, 0}, {-x, y, z}, {x, y, z}, color);
  addQuad(mesh, {-x, -y, 0}, {x, -y, 0}, {x, -y, z}, {-x, -y, z}, color);

  return mesh;
}

MeshData plane(const glm::vec2& size, const glm::vec3& color)
{
  const float x = size.x * 0.5f;
  const float y = size.y * 0.5f;

  MeshData mesh;
  addQuad(mesh, {-x, -y, 0}, {x, -y, 0}, {x, y, 0}, {-x, y, 0}, color);
  return mesh;
}

MeshData cylinder(float radius, float height, int segments, const glm::vec3& color)
{
  segments = std::max(segments, 3);

  MeshData mesh;

  // Side wall: one ring at the base and one at the top, smooth normals.
  const auto base = static_cast<uint32_t>(mesh.vertices.size());
  for (int i = 0; i < segments; ++i)
  {
    const float angle = kTwoPi * static_cast<float>(i) / segments;
    const glm::vec3 normal{std::cos(angle), std::sin(angle), 0.0f};
    const glm::vec3 rim = normal * radius;

    mesh.vertices.push_back({rim, normal, color});
    mesh.vertices.push_back({rim + glm::vec3(0, 0, height), normal, color});
  }

  for (int i = 0; i < segments; ++i)
  {
    const uint32_t bottom = base + static_cast<uint32_t>(i) * 2;
    const uint32_t top = bottom + 1;
    const uint32_t nextBottom =
        base + static_cast<uint32_t>((i + 1) % segments) * 2;
    const uint32_t nextTop = nextBottom + 1;

    mesh.indices.insert(mesh.indices.end(),
                        {bottom, nextBottom, nextTop, bottom, nextTop, top});
  }

  addCap(mesh, radius, height, segments, true, color);
  addCap(mesh, radius, 0.0f, segments, false, color);
  return mesh;
}

MeshData cone(float radius, float height, int segments, const glm::vec3& color)
{
  segments = std::max(segments, 3);

  MeshData mesh;

  // Side normals tilt up by the slope; each face gets its own apex vertex so
  // the normal at the tip matches the face it belongs to.
  const float slope = radius / height;
  for (int i = 0; i < segments; ++i)
  {
    const float a0 = kTwoPi * static_cast<float>(i) / segments;
    const float a1 = kTwoPi * static_cast<float>(i + 1) / segments;
    const float mid = (a0 + a1) * 0.5f;

    const auto sideNormal = [slope](float angle)
    {
      return glm::normalize(
          glm::vec3(std::cos(angle), std::sin(angle), slope));
    };

    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(
        {{radius * std::cos(a0), radius * std::sin(a0), 0.0f},
         sideNormal(a0),
         color});
    mesh.vertices.push_back(
        {{radius * std::cos(a1), radius * std::sin(a1), 0.0f},
         sideNormal(a1),
         color});
    mesh.vertices.push_back({{0.0f, 0.0f, height}, sideNormal(mid), color});
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
  }

  addCap(mesh, radius, 0.0f, segments, false, color);
  return mesh;
}

MeshData sphere(float radius, int segments, int rings, const glm::vec3& color)
{
  segments = std::max(segments, 3);
  rings = std::max(rings, 2);

  MeshData mesh;
  const glm::vec3 center{0.0f, 0.0f, radius};

  // (rings + 1) latitude rows x (segments + 1) columns; the seam column is
  // duplicated so every quad indexes a full row.
  for (int ring = 0; ring <= rings; ++ring)
  {
    const float polar = glm::pi<float>() * static_cast<float>(ring) / rings;
    for (int segment = 0; segment <= segments; ++segment)
    {
      const float azimuth = kTwoPi * static_cast<float>(segment) / segments;
      const glm::vec3 normal{std::sin(polar) * std::cos(azimuth),
                             std::sin(polar) * std::sin(azimuth),
                             std::cos(polar)};
      mesh.vertices.push_back({center + normal * radius, normal, color});
    }
  }

  const auto columns = static_cast<uint32_t>(segments + 1);
  for (uint32_t ring = 0; ring < static_cast<uint32_t>(rings); ++ring)
  {
    for (uint32_t segment = 0; segment < static_cast<uint32_t>(segments);
         ++segment)
    {
      const uint32_t a = ring * columns + segment;
      const uint32_t b = a + columns;

      // Row `ring` is nearer +Z; wind so the outside faces front.
      mesh.indices.insert(mesh.indices.end(),
                          {a, b, b + 1, a, b + 1, a + 1});
    }
  }

  return mesh;
}

MeshData prism(const glm::vec3& size, const glm::vec3& color)
{
  const float x = size.x * 0.5f;
  const float y = size.y * 0.5f;
  const float z = size.z;

  MeshData mesh;

  // Two sloped roof faces meeting at the ridge (Y = 0, Z = z).
  addQuad(mesh, {-x, -y, 0}, {x, -y, 0}, {x, 0, z}, {-x, 0, z}, color);
  addQuad(mesh, {x, y, 0}, {-x, y, 0}, {-x, 0, z}, {x, 0, z}, color);
  // Gable ends.
  addTriangle(mesh, {x, -y, 0}, {x, y, 0}, {x, 0, z}, color);
  addTriangle(mesh, {-x, y, 0}, {-x, -y, 0}, {-x, 0, z}, color);
  // Underside.
  addQuad(mesh, {-x, y, 0}, {x, y, 0}, {x, -y, 0}, {-x, -y, 0}, color);

  return mesh;
}

void append(MeshData& target, const MeshData& source, const glm::mat4& transform)
{
  const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));
  const auto base = static_cast<uint32_t>(target.vertices.size());

  target.vertices.reserve(target.vertices.size() + source.vertices.size());
  for (const MeshVertex& vertex : source.vertices)
  {
    target.vertices.push_back(
        {glm::vec3(transform * glm::vec4(vertex.position, 1.0f)),
         glm::normalize(normalMatrix * vertex.normal),
         vertex.color});
  }

  target.indices.reserve(target.indices.size() + source.indices.size());
  for (const uint32_t index : source.indices)
    target.indices.push_back(base + index);
}

} // namespace sfs::meshes
