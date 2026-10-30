#pragma once
#include <sfs/sfs.h>

#include "engine/core/rendering/meshPrimitives.h"

#include "glm/glm/gtc/matrix_transform.hpp"

#include <cmath>

// Glowfall Isle's models, built in code (a MeshSource): every scene of the
// game can use them by name ("glowfall:..."), and the editor lists them in its
// mesh pickers like the built-in shapes. Low-poly, coloured per vertex (a
// material multiplies the vertex colour), each standing on Z = 0.
class GlowfallModels : public sfs::MeshSource
{
public:
  void build(sfs::MeshLibrary& library) override
  {
    library.add(island(), "glowfall:island");
    library.add(turf(), "glowfall:turf");
    library.add(hero(), "glowfall:hero");
    library.add(pin(), "glowfall:pin");
    library.add(sfs::meshes::sphere(0.45f, 20, 12), "glowfall:ball");
    library.add(tree(), "glowfall:tree");
    library.add(lanternPost(), "glowfall:lanternPost");
    library.add(
        sfs::meshes::box({0.34f, 0.34f, 0.42f}), "glowfall:lanternGlass");
    library.add(crystal(), "glowfall:crystal");
    library.add(spark(), "glowfall:spark");
    library.add(sfs::meshes::cylinder(3.0f, 0.06f, 40), "glowfall:disc");
    library.add(sfs::meshes::cylinder(0.5f, 1.0f, 16), "glowfall:column");
    library.add(sfs::meshes::sphere(0.6f, 32, 20), "glowfall:orb");
    library.add(fountain(), "glowfall:fountain");
  }

private:
  using Mat = glm::mat4;

  static Mat
  at(glm::vec3 offset, glm::vec3 scale = glm::vec3(1.0f), float yaw = 0.0f)
  {
    return glm::scale(
        glm::rotate(glm::translate(Mat(1.0f), offset), yaw, glm::vec3(0, 0, 1)),
        scale);
  }

  // The island's outline: a rounded square, wobbling, `scale` of the way out
  // from its middle (point i of kRing around).
  static constexpr int kRing = 96;      // points around
  static constexpr float kHalf = 26.0f; // half its size
  static constexpr float kTurfEdge = 0.85f; // where the turf gives way to sand
  static glm::vec2 rim(int i, float scale)
  {
    const float a = 6.2831853f * static_cast<float>(i) / kRing;
    const float c = std::cos(a), s = std::sin(a);
    // A superellipse (exponent 6): nearly square, round corners.
    const float r = kHalf / std::pow(std::pow(std::abs(c), 6.0f) +
                                         std::pow(std::abs(s), 6.0f),
                                     1.0f / 6.0f);
    const float wobble = 1.0f + 0.02f * std::sin(a * 7.0f) +
                         0.015f * std::sin(a * 13.0f + 1.0f);
    return glm::vec2(c, s) * r * wobble * scale;
  }

  // The island's turf: its flat top inside the sand, a hair above it, for the
  // grass that covers the island to grow on.
  static sfs::MeshData turf()
  {
    sfs::MeshData mesh;
    sfs::MeshVertex middle;
    middle.position = glm::vec3(0.0f, 0.0f, 0.004f);
    middle.normal = glm::vec3(0.0f, 0.0f, 1.0f);
    middle.color = glm::vec3(0.42f, 0.62f, 0.3f);
    mesh.vertices.push_back(middle);
    for (int i = 0; i < kRing; ++i)
    {
      sfs::MeshVertex v = middle;
      v.position = glm::vec3(rim(i, kTurfEdge), 0.004f);
      mesh.vertices.push_back(v);
    }
    for (uint32_t i = 0; i < kRing; ++i)
      mesh.indices.insert(mesh.indices.end(),
                          {0u, 1u + i, 1u + (i + 1) % kRing});
    return mesh;
  }

  // A plateau in the sea: a flat top (sand at its rim, turf inside) and cliffs
  // falling away below the waterline, ragged by a little noise.
  static sfs::MeshData island()
  {
    sfs::MeshData mesh;
    const glm::vec3 turf(0.42f, 0.62f, 0.3f), sand(0.86f, 0.78f, 0.56f),
        cliff(0.46f, 0.42f, 0.4f), deep(0.3f, 0.28f, 0.3f);

    // Rings from the centre out: turf, then sand, then the cliff down.
    struct Ring
    {
      float scale, z;
      glm::vec3 colour;
    };
    const Ring rings[] = {{0.0f, 0.0f, turf},
                          {kTurfEdge, 0.0f, turf},
                          {0.95f, 0.0f, sand},
                          {1.0f, -0.15f, sand},
                          {1.04f, -1.6f, cliff},
                          {1.1f, -4.0f, deep}};
    constexpr int kRings = sizeof(rings) / sizeof(rings[0]);
    for (int r = 0; r < kRings; ++r)
      for (int i = 0; i < kRing; ++i)
      {
        sfs::MeshVertex v;
        const glm::vec2 p = rim(i, rings[r].scale);
        v.position = glm::vec3(p, rings[r].z);
        v.color = rings[r].colour;
        mesh.vertices.push_back(v);
      }
    for (int r = 0; r + 1 < kRings; ++r)
      for (int i = 0; i < kRing; ++i)
      {
        const uint32_t a = r * kRing + i, b = r * kRing + (i + 1) % kRing;
        const uint32_t c = a + kRing, d = b + kRing;
        mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
      }
    // Normals: smooth, from the faces around each point.
    for (sfs::MeshVertex& v : mesh.vertices)
      v.normal = glm::vec3(0.0f);
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
    {
      sfs::MeshVertex& a = mesh.vertices[mesh.indices[t]];
      sfs::MeshVertex& b = mesh.vertices[mesh.indices[t + 1]];
      sfs::MeshVertex& c = mesh.vertices[mesh.indices[t + 2]];
      const glm::vec3 n =
          glm::cross(b.position - a.position, c.position - a.position);
      a.normal += n;
      b.normal += n;
      c.normal += n;
    }
    for (sfs::MeshVertex& v : mesh.vertices)
      v.normal = glm::length(v.normal) > 0.0f ? glm::normalize(v.normal)
                                              : glm::vec3(0, 0, 1);
    return mesh;
  }

  // The hero: a round little explorer in a teal coat, facing +X.
  static sfs::MeshData hero()
  {
    using namespace sfs::meshes;
    sfs::MeshData mesh;
    const glm::vec3 coat(0.15f, 0.62f, 0.66f), skin(0.98f, 0.82f, 0.68f),
        dark(0.08f, 0.08f, 0.12f), scarf(0.95f, 0.45f, 0.2f),
        boot(0.3f, 0.2f, 0.15f);
    append(mesh, cylinder(0.12f, 0.35f, 10, boot), at({0.0f, 0.17f, 0.0f}));
    append(mesh, cylinder(0.12f, 0.35f, 10, boot), at({0.0f, -0.17f, 0.0f}));
    append(mesh, cylinder(0.36f, 0.75f, 18, coat), at({0.0f, 0.0f, 0.3f}));
    append(mesh,
           sphere(0.36f, 18, 10, coat),
           at({0.0f, 0.0f, 0.68f}, {1.0f, 1.0f, 0.6f}));
    append(mesh, cylinder(0.3f, 0.14f, 18, scarf), at({0.0f, 0.0f, 1.0f}));
    append(mesh, sphere(0.32f, 20, 12, skin), at({0.02f, 0.0f, 1.08f}));
    append(mesh, sphere(0.05f, 8, 6, dark), at({0.3f, 0.11f, 1.28f}));
    append(mesh, sphere(0.05f, 8, 6, dark), at({0.3f, -0.11f, 1.28f}));
    append(mesh, cone(0.34f, 0.32f, 18, coat), at({0.0f, 0.0f, 1.42f}));
    append(mesh, sphere(0.08f, 8, 6, scarf), at({0.0f, 0.0f, 1.72f}));
    return mesh;
  }

  // A bowling pin, white with a red band.
  static sfs::MeshData pin()
  {
    using namespace sfs::meshes;
    sfs::MeshData mesh;
    const glm::vec3 white(0.95f, 0.94f, 0.9f), red(0.85f, 0.15f, 0.15f);
    append(mesh, cylinder(0.12f, 0.1f, 14, white));
    append(mesh,
           sphere(0.17f, 14, 10, white),
           at({0.0f, 0.0f, 0.05f}, {1.0f, 1.0f, 1.3f}));
    append(mesh, cylinder(0.09f, 0.3f, 14, white), at({0.0f, 0.0f, 0.42f}));
    append(mesh, cylinder(0.095f, 0.06f, 14, red), at({0.0f, 0.0f, 0.6f}));
    append(mesh, sphere(0.11f, 14, 10, white), at({0.0f, 0.0f, 0.7f}));
    return mesh;
  }

  // A pine: a trunk and three tiers of needles.
  static sfs::MeshData tree()
  {
    using namespace sfs::meshes;
    sfs::MeshData mesh;
    const glm::vec3 bark(0.38f, 0.25f, 0.16f), needles(0.13f, 0.38f, 0.22f);
    append(mesh, cylinder(0.25f, 1.6f, 10, bark));
    append(mesh, cone(1.6f, 1.8f, 12, needles), at({0.0f, 0.0f, 1.2f}));
    append(mesh, cone(1.25f, 1.6f, 12, needles * 1.1f), at({0.0f, 0.0f, 2.2f}));
    append(mesh, cone(0.85f, 1.4f, 12, needles * 1.2f), at({0.0f, 0.0f, 3.1f}));
    return mesh;
  }

  // A lamp post; its glass is a separate mesh with a glowing material.
  static sfs::MeshData lanternPost()
  {
    using namespace sfs::meshes;
    sfs::MeshData mesh;
    const glm::vec3 iron(0.12f, 0.12f, 0.14f);
    append(mesh, cylinder(0.2f, 0.15f, 12, iron));
    append(mesh, cylinder(0.06f, 1.85f, 8, iron), at({0.0f, 0.0f, 0.15f}));
    append(mesh, box({0.42f, 0.42f, 0.06f}, iron), at({0.0f, 0.0f, 2.0f}));
    append(mesh,
           cone(0.32f, 0.25f, 4, iron),
           at({0.0f, 0.0f, 2.47f}, glm::vec3(1.0f), 0.785f));
    return mesh;
  }

  // A cluster of crystal shards.
  static sfs::MeshData crystal()
  {
    using namespace sfs::meshes;
    sfs::MeshData mesh;
    const float tilts[][3] = {{0.0f, 0.0f, 1.0f},
                              {0.4f, 0.3f, 0.7f},
                              {-0.35f, 0.2f, 0.6f},
                              {0.1f, -0.4f, 0.55f}};
    for (const auto& t : tilts)
    {
      Mat m = glm::translate(Mat(1.0f), {t[0] * 0.4f, t[1] * 0.4f, 0.0f});
      m = glm::rotate(m, t[0] * 0.6f, {0, 1, 0});
      m = glm::rotate(m, -t[1] * 0.6f, {1, 0, 0});
      append(mesh,
             cylinder(0.18f, 1.4f * t[2], 6),
             m * glm::scale(Mat(1.0f), {1, 1, 1}));
      append(mesh,
             cone(0.18f, 0.35f, 6),
             m * glm::translate(Mat(1.0f), {0, 0, 1.4f * t[2]}));
    }
    return mesh;
  }

  // A spark: a little star (two pyramids), centred on its origin.
  static sfs::MeshData spark()
  {
    using namespace sfs::meshes;
    sfs::MeshData mesh;
    append(mesh, cone(0.3f, 0.35f, 4));
    append(mesh,
           cone(0.3f, 0.35f, 4),
           glm::rotate(Mat(1.0f), 3.14159265f, {1, 0, 0}));
    return mesh;
  }

  // A stone basin with a spout in the middle.
  static sfs::MeshData fountain()
  {
    using namespace sfs::meshes;
    sfs::MeshData mesh;
    const glm::vec3 stone(0.8f, 0.78f, 0.74f);
    append(mesh, cylinder(2.4f, 0.45f, 32, stone));
    append(mesh, cylinder(0.3f, 1.3f, 12, stone));
    append(mesh, cylinder(0.55f, 0.12f, 16, stone), at({0.0f, 0.0f, 1.3f}));
    return mesh;
  }
};
