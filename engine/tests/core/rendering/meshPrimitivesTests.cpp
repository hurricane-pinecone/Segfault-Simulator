#include "../../testHarness.h"

#include <engine/core/rendering/meshPrimitives.h>

#include <glm/glm/ext/matrix_transform.hpp>
#include <glm/glm/geometric.hpp>

using namespace sfs;

namespace
{
bool nearVec(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f)
{
  return testing::approx(a.x, b.x, eps) && testing::approx(a.y, b.y, eps) &&
         testing::approx(a.z, b.z, eps);
}

// Every index is in range and the list is whole triangles.
bool wellFormed(const MeshData& mesh)
{
  if (mesh.indices.size() % 3 != 0)
    return false;
  for (const uint32_t index : mesh.indices)
    if (index >= mesh.vertices.size())
      return false;
  return true;
}

// Every triangle's winding agrees with its vertex normals and faces away from
// `inside` (a point within a convex shape), i.e. front faces point outward.
bool facesOutward(const MeshData& mesh, const glm::vec3& inside)
{
  for (std::size_t i = 0; i < mesh.indices.size(); i += 3)
  {
    const glm::vec3& a = mesh.vertices[mesh.indices[i]].position;
    const glm::vec3& b = mesh.vertices[mesh.indices[i + 1]].position;
    const glm::vec3& c = mesh.vertices[mesh.indices[i + 2]].position;
    const glm::vec3 faceNormal = glm::cross(b - a, c - a);
    const glm::vec3 centroid = (a + b + c) / 3.0f;

    if (glm::dot(faceNormal, centroid - inside) <= 0.0f)
      return false;
    if (glm::dot(faceNormal, mesh.vertices[mesh.indices[i]].normal) <= 0.0f)
      return false;
  }
  return true;
}

bool unitNormals(const MeshData& mesh)
{
  for (const MeshVertex& vertex : mesh.vertices)
    if (!testing::approx(glm::length(vertex.normal), 1.0f, 1e-3f))
      return false;
  return true;
}
} // namespace

int main()
{
  TEST("box should be 6 flat faces sitting on Z = 0")
  {
    const MeshData mesh = meshes::box({2.0f, 4.0f, 3.0f});
    CHECK(mesh.vertices.size() == 24u);
    CHECK(mesh.indices.size() == 36u);
    CHECK(wellFormed(mesh));
    CHECK(unitNormals(mesh));
    CHECK(nearVec(mesh.bounds().min, {-1.0f, -2.0f, 0.0f}));
    CHECK(nearVec(mesh.bounds().max, {1.0f, 2.0f, 3.0f}));
  }

  TEST("box faces should wind counter-clockwise from outside")
  {
    CHECK(facesOutward(meshes::box({2.0f, 2.0f, 2.0f}), {0.0f, 0.0f, 1.0f}));
  }

  TEST("box should carry the requested colour on every vertex")
  {
    const glm::vec3 red{1.0f, 0.0f, 0.0f};
    const MeshData mesh = meshes::box({1.0f, 1.0f, 1.0f}, red);
    bool allRed = true;
    for (const MeshVertex& vertex : mesh.vertices)
      allRed = allRed && nearVec(vertex.color, red);
    CHECK(allRed);
  }

  TEST("plane should face up")
  {
    const MeshData mesh = meshes::plane({10.0f, 6.0f});
    CHECK(mesh.indices.size() == 6u);
    CHECK(nearVec(mesh.vertices[0].normal, {0.0f, 0.0f, 1.0f}));
    CHECK(nearVec(mesh.bounds().max, {5.0f, 3.0f, 0.0f}));
  }

  TEST("cylinder, cone, sphere and prism should face outward")
  {
    const MeshData cylinder = meshes::cylinder(1.0f, 2.0f, 12);
    const MeshData cone = meshes::cone(1.0f, 2.0f, 12);
    const MeshData sphere = meshes::sphere(1.0f, 12, 8);
    const MeshData prism = meshes::prism({2.0f, 2.0f, 1.0f});

    CHECK(wellFormed(cylinder));
    CHECK(wellFormed(cone));
    CHECK(wellFormed(sphere));
    CHECK(wellFormed(prism));
    CHECK(facesOutward(cylinder, {0.0f, 0.0f, 1.0f}));
    CHECK(facesOutward(cone, {0.0f, 0.0f, 0.5f}));
    CHECK(facesOutward(prism, {0.0f, 0.0f, 0.3f}));
    CHECK(unitNormals(cylinder));
    CHECK(unitNormals(cone));
    CHECK(unitNormals(sphere));
  }

  TEST("sphere should rest on Z = 0")
  {
    const Aabb bounds = meshes::sphere(1.5f, 16, 8).bounds();
    CHECK(testing::approx(bounds.min.z, 0.0f));
    CHECK(testing::approx(bounds.max.z, 3.0f));
  }

  TEST("every shape should have texture coordinates across the whole image")
  {
    for (const MeshData& mesh : {meshes::box({1.0f, 1.0f, 1.0f}),
                                 meshes::plane({1.0f, 1.0f}),
                                 meshes::cylinder(1.0f, 2.0f, 12),
                                 meshes::cone(1.0f, 2.0f, 12),
                                 meshes::sphere(1.0f, 12, 8),
                                 meshes::prism({1.0f, 1.0f, 1.0f})})
    {
      glm::vec2 low(1.0f), high(0.0f);
      for (const MeshVertex& vertex : mesh.vertices)
      {
        low = glm::min(low, vertex.uv);
        high = glm::max(high, vertex.uv);
      }
      CHECK(low.x <= 0.0f);
      CHECK(low.y <= 0.0f);
      CHECK(high.x >= 1.0f);
      CHECK(high.y >= 1.0f);
    }
  }

  TEST("append should keep texture coordinates")
  {
    MeshData combined;
    const MeshData sphere = meshes::sphere(1.0f, 8, 4);
    meshes::append(combined, sphere, glm::mat4(1.0f));
    bool same = combined.vertices.size() == sphere.vertices.size();
    for (std::size_t i = 0; same && i < sphere.vertices.size(); ++i)
      same = combined.vertices[i].uv == sphere.vertices[i].uv;
    CHECK(same);
  }

  TEST("too few segments should clamp to a valid shape")
  {
    const MeshData cylinder = meshes::cylinder(1.0f, 1.0f, 1);
    CHECK(wellFormed(cylinder));
    CHECK(!cylinder.empty());
  }

  TEST("append should transform positions and offset indices")
  {
    MeshData combined = meshes::box({1.0f, 1.0f, 1.0f});
    const std::size_t firstVertices = combined.vertices.size();

    meshes::append(combined,
                   meshes::box({1.0f, 1.0f, 1.0f}),
                   glm::translate(glm::mat4(1.0f), {10.0f, 0.0f, 0.0f}));

    CHECK(combined.vertices.size() == firstVertices * 2);
    CHECK(combined.indices.size() == 72u);
    CHECK(wellFormed(combined));
    CHECK(testing::approx(combined.bounds().max.x, 10.5f));
  }

  TEST("append should keep normals unit length under non-uniform scale")
  {
    MeshData combined;
    meshes::append(combined,
                   meshes::sphere(1.0f, 8, 6),
                   glm::scale(glm::mat4(1.0f), {4.0f, 1.0f, 0.5f}));
    CHECK(unitNormals(combined));
  }

  return testing::report("meshPrimitivesTests");
}
