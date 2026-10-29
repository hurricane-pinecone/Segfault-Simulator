#include "../../../testHarness.h"

#include <engine/core/rendering/3d/emissionLights.h>
#include <engine/core/rendering/3d/materialGraphCompiler.h>
#include <engine/core/rendering/3d/meshPrimitives.h>

#include "glm/glm/geometric.hpp"

#include <cmath>
#include <vector>

using namespace sfs;

namespace
{

float meshArea(const MeshData& mesh)
{
  float area = 0.0f;
  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
  {
    const glm::vec3& a = mesh.vertices[mesh.indices[i]].position;
    area += 0.5f * glm::length(glm::cross(
                       mesh.vertices[mesh.indices[i + 1]].position - a,
                       mesh.vertices[mesh.indices[i + 2]].position - a));
  }
  return area;
}

std::vector<GlowPoint> glowing(const std::vector<SurfaceSample>& samples,
                               glm::vec3 emission,
                               glm::vec3 offset = glm::vec3(0.0f))
{
  std::vector<GlowPoint> points;
  for (const SurfaceSample& sample : samples)
    points.push_back({sample.position + offset, sample.area, emission});
  return points;
}

} // namespace

int main()
{
  TEST("samples should cover a surface evenly, standing for all its area, the "
       "same every time")
  {
    const MeshData plane = meshes::plane({4.0f, 2.0f});
    const std::vector<SurfaceSample> samples =
        sampleSurface(plane.vertices, plane.indices, 64);
    CHECK(samples.size() == 64u);
    float area = 0.0f;
    int left = 0;
    bool onIt = true;
    for (const SurfaceSample& sample : samples)
    {
      area += sample.area;
      onIt = onIt && std::abs(sample.position.z) < 1e-5f &&
             std::abs(sample.position.x) <= 2.0f &&
             std::abs(sample.position.y) <= 1.0f &&
             testing::approx(sample.normal.z, 1.0f);
      left += sample.position.x < 0.0f;
    }
    CHECK(testing::approx(area, 8.0f, 1e-3f));
    CHECK(onIt);
    CHECK(left >= 26 && left <= 38); // about half on each side
    const std::vector<SurfaceSample> again =
        sampleSurface(plane.vertices, plane.indices, 64);
    bool same = true;
    for (std::size_t i = 0; i < samples.size(); ++i)
      same = same && samples[i].position == again[i].position;
    CHECK(same);

    const MeshData sphere = meshes::sphere(0.5f, 24, 16, glm::vec3(1.0f));
    float sphereArea = 0.0f;
    for (const SurfaceSample& sample :
         sampleSurface(sphere.vertices, sphere.indices, 100))
      sphereArea += sample.area;
    CHECK(testing::approx(sphereArea, meshArea(sphere), 1e-3f));

    // More area, more samples, within bounds.
    CHECK(surfaceSampleCount(0.001f) == 16u);
    CHECK(surfaceSampleCount(1.0f) > surfaceSampleCount(0.5f));
    CHECK(surfaceSampleCount(1000.0f) == 128u);
    CHECK(sampleSurface(plane.vertices, {}, 10).empty());
  }

  TEST("a small glowing object should give one light, where and in the colour "
       "it glows")
  {
    const MeshData sphere = meshes::sphere(0.5f, 24, 16, glm::vec3(1.0f));
    const std::vector<SurfaceSample> samples = sampleSurface(
        sphere.vertices, sphere.indices, surfaceSampleCount(meshArea(sphere)));
    std::vector<PointLight3D> lights;
    const glm::vec3 at(3.0f, -2.0f, 1.0f);
    emissionLights(
        glowing(samples, {4.0f, 2.0f, 0.0f}, at), 1.0f, 1.0f, lights);
    CHECK(lights.size() == 1u);
    if (lights.size() == 1u)
    {
      const PointLight3D& light = lights[0];
      // At the sphere's centre (it sits on the ground, its centre a radius up).
      CHECK(glm::length(light.position - (at + glm::vec3(0.0f, 0.0f, 0.5f))) <
            0.05f);
      CHECK(testing::approx(light.color.r, 1.0f) &&
            testing::approx(light.color.g, 0.5f) &&
            testing::approx(light.color.b, 0.0f));
      CHECK(testing::approx(light.intensity, 4.0f, 1e-3f));
      // Reach: where brightness x area / (pi d^2) falls to 0.02.
      const float amount = 4.0f * meshArea(sphere);
      CHECK(testing::approx(
          light.radius, std::sqrt(amount / (3.14159265f * 0.02f)), 1e-2f));
      // It casts shadows, but not from the sphere itself: everything of it
      // is within shadowNear of the light, along every axis.
      CHECK(light.castsShadow);
      CHECK(light.shadowNear >= 0.5f && light.shadowNear < 0.7f);
    }

    // Strength and range scale it; brighter lights more strongly, not
    // farther than more glowing surface would.
    std::vector<PointLight3D> tuned;
    emissionLights(glowing(samples, {4.0f, 2.0f, 0.0f}, at), 0.5f, 2.0f, tuned);
    CHECK(tuned.size() == 1u &&
          testing::approx(tuned[0].intensity, 2.0f, 1e-3f) &&
          testing::approx(tuned[0].radius, lights[0].radius * 2.0f, 1e-2f));

    // Nothing for something that doesn't glow, or with strength 0.
    std::vector<PointLight3D> none;
    emissionLights(glowing(samples, glm::vec3(0.0f)), 1.0f, 1.0f, none);
    emissionLights(glowing(samples, glm::vec3(1.0f)), 0.0f, 1.0f, none);
    CHECK(none.empty());
  }

  TEST("a light should sit where the glow is, not where the object is")
  {
    // A plane glowing only on its right half.
    const MeshData plane = meshes::plane({1.0f, 1.0f});
    std::vector<GlowPoint> points = glowing(
        sampleSurface(plane.vertices, plane.indices, 64), glm::vec3(1.0f));
    for (GlowPoint& point : points)
      if (point.position.x < 0.0f)
        point.emission = glm::vec3(0.0f);
    std::vector<PointLight3D> lights;
    emissionLights(points, 1.0f, 1.0f, lights);
    CHECK(lights.size() == 1u && lights[0].position.x > 0.15f);
  }

  TEST(
      "a big glowing surface should be lit from all over it, by several lights")
  {
    MeshData lake = meshes::plane({40.0f, 40.0f});
    const std::vector<SurfaceSample> samples =
        sampleSurface(lake.vertices, lake.indices, 128);
    std::vector<PointLight3D> lights;
    emissionLights(
        glowing(samples, glm::vec3(0.5f, 0.2f, 0.0f)), 1.0f, 1.0f, lights);
    CHECK(lights.size() >= 8u && lights.size() <= 64u);
    glm::vec3 low(1e9f), high(-1e9f);
    float total = 0.0f;
    for (const PointLight3D& light : lights)
    {
      low = glm::min(low, light.position);
      high = glm::max(high, light.position);
      total += light.intensity;
    }
    CHECK(high.x - low.x > 25.0f && high.y - low.y > 25.0f);
    // Each as bright as the glow (the same all over).
    CHECK(testing::approx(
        total / static_cast<float>(lights.size()), 0.5f, 1e-3f));
  }

  TEST("only a graph material with emission that isn't black should be able to "
       "glow")
  {
    namespace graph = materialGraph;
    const auto compiled = [](const graph::Graph& g)
    {
      const graph::CompiledGraph result = graph::compile(g);
      MeshMaterial material;
      material.shader = result.shader;
      material.shaderParameters = result.parameters;
      material.shaderTextures.resize(result.textures.size());
      return material;
    };
    CHECK(!canGlow(MeshMaterial{})); // the standard surface

    graph::Graph dark;
    const graph::NodeId darkOut = dark.add(std::string(graph::kOutputType), {});
    CHECK(!canGlow(compiled(dark))); // emission black, as a new graph's is

    graph::Graph lit = dark;
    lit.find(darkOut)->inputs[graph::kEmission] =
        glm::vec4(0.0f, 0.2f, 0.0f, 1.0f);
    CHECK(canGlow(compiled(lit)));

    // Linked to something that varies: it might glow, so it can.
    graph::Graph pulsing = dark;
    const graph::NodeId time = pulsing.add("time", {});
    pulsing.connect(time, 1, darkOut, graph::kEmission);
    CHECK(canGlow(compiled(pulsing)));

    // Its values decide: the same graph with the value at 0 can't.
    MeshMaterial material = compiled(lit);
    for (glm::vec4& value : material.shaderParameters)
      value = glm::vec4(0.0f);
    CHECK(!canGlow(material));
  }

  return testing::report("emissionLightsTests");
}
