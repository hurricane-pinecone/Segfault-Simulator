#include "../../../testHarness.h"

#include <engine/core/rendering/3d/camera3D.h>
#include <engine/core/rendering/3d/lightClusters.h>

#include <algorithm>
#include <cmath>
#include <random>

using namespace sfs;

namespace
{

struct Setup
{
  ClusterGrid grid;
  ClusterView view;
};

Setup setup(const Camera3D& camera, glm::vec2 viewport)
{
  Setup s;
  s.grid =
      ClusterGrid::forViewport(viewport, camera.nearPlane, camera.farPlane);
  s.view.viewProjection = camera.viewProjection(viewport.x / viewport.y);
  s.view.viewport = viewport;
  s.view.cameraPosition = camera.position();
  s.view.forward = camera.viewDirection();
  return s;
}

bool listed(const ClusterLights& lists, uint32_t cluster, uint32_t light)
{
  const glm::uvec2 range = lists.ranges[cluster];
  return std::find(lists.indices.begin() + range.x,
                   lists.indices.begin() + range.x + range.y,
                   light) != lists.indices.begin() + range.x + range.y;
}

// Every point a light reaches that the view sees should find the light in
// its cluster (as the shader looks it up).
bool everyReachedPointFindsItsLight(const Setup& s,
                                    const std::vector<PointLight3D>& lights,
                                    uint32_t seed)
{
  ClusterLights lists;
  assignLights(s.grid, s.view, lights, lists);
  std::mt19937 random(seed);
  std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
  int checked = 0;
  for (uint32_t i = 0; i < lights.size(); ++i)
    for (int sample = 0; sample < 40; ++sample)
    {
      glm::vec3 offset(unit(random), unit(random), unit(random));
      if (glm::length(offset) > 1.0f)
        continue;
      const glm::vec3 point = lights[i].position + offset * lights[i].radius;
      const glm::vec4 clip = s.view.viewProjection * glm::vec4(point, 1.0f);
      if (clip.w <= 0.0f)
        continue;
      const glm::vec3 ndc = glm::vec3(clip) / clip.w;
      if (std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f || ndc.z < 0.0f ||
          ndc.z > 1.0f)
        continue; // not on screen
      const glm::vec2 pixel((ndc.x * 0.5f + 0.5f) * s.view.viewport.x,
                            (0.5f - ndc.y * 0.5f) * s.view.viewport.y);
      const float depth =
          glm::dot(point - s.view.cameraPosition, s.view.forward);
      const uint32_t cluster =
          s.grid.index(s.grid.tileOf(pixel), s.grid.sliceOf(depth));
      ++checked;
      if (!listed(lists, cluster, i))
        return false;
    }
  return checked > 100;
}

std::vector<PointLight3D> scatter(uint32_t count, float spread, uint32_t seed)
{
  std::mt19937 random(seed);
  std::uniform_real_distribution<float> place(-spread, spread);
  std::uniform_real_distribution<float> reach(0.5f, 6.0f);
  std::vector<PointLight3D> lights(count);
  for (PointLight3D& light : lights)
  {
    light.position = {
        place(random), place(random), place(random) * 0.2f + 2.0f};
    light.radius = reach(random);
  }
  return lights;
}

} // namespace

int main()
{
  TEST("a grid should cover the viewport in tiles and find a pixel's and a "
       "depth's place")
  {
    const ClusterGrid grid =
        ClusterGrid::forViewport({1920.0f, 1080.0f}, 0.1f, 500.0f);
    CHECK(grid.tilePixels == 64.0f);
    CHECK(grid.size.x == 30u);
    CHECK(grid.size.y == 17u);
    // A small view gets smaller tiles, so its lights still split up.
    const ClusterGrid small =
        ClusterGrid::forViewport({256.0f, 256.0f}, 0.1f, 500.0f);
    CHECK(small.tilePixels == 16.0f);
    CHECK(small.size.x == 16u);
    CHECK(grid.tileOf({0.0f, 0.0f}) == glm::uvec2(0, 0));
    CHECK(grid.tileOf({1919.0f, 1079.0f}) == glm::uvec2(29, 16));
    CHECK(grid.tileOf({5000.0f, -10.0f}) == glm::uvec2(29, 0)); // clamped
    CHECK(grid.sliceOf(0.05f) == 0u);
    CHECK(grid.sliceOf(1000.0f) == grid.size.z - 1);
    // Slices grow with depth: equal ratios, equal steps (to within the one a
    // slice boundary falling between them makes).
    const int nearSteps = int(grid.sliceOf(10.0f)) - int(grid.sliceOf(1.0f));
    const int farSteps = int(grid.sliceOf(100.0f)) - int(grid.sliceOf(10.0f));
    CHECK(std::abs(nearSteps - farSteps) <= 1);
    CHECK(nearSteps >= 5);
  }

  TEST("every point a light reaches should find it in its cluster, in "
       "perspective")
  {
    Camera3D camera;
    camera.target = {0.0f, 0.0f, 0.0f};
    camera.distance = 30.0f;
    camera.pitch = 0.5f;
    camera.yaw = 0.3f;
    const Setup s = setup(camera, {1280.0f, 720.0f});
    CHECK(everyReachedPointFindsItsLight(s, scatter(300, 25.0f, 1), 7));
  }

  TEST("and with an orthographic camera, and lights around and behind it")
  {
    Camera3D camera;
    camera.projection = Camera3D::Projection::Orthographic;
    camera.orthoHeight = 30.0f;
    camera.distance = 40.0f;
    const Setup s = setup(camera, {1000.0f, 800.0f});
    CHECK(everyReachedPointFindsItsLight(s, scatter(300, 25.0f, 2), 8));

    Camera3D close;
    close.distance = 3.0f;
    const Setup near = setup(close, {800.0f, 600.0f});
    CHECK(everyReachedPointFindsItsLight(near, scatter(200, 8.0f, 3), 9));
  }

  TEST("lights off screen, or past the far plane, should be in no cluster")
  {
    Camera3D camera;
    camera.distance = 20.0f;
    camera.farPlane = 100.0f;
    const Setup s = setup(camera, {800.0f, 600.0f});
    std::vector<PointLight3D> lights(2);
    lights[0].position =
        camera.position() - camera.viewDirection() * 50.0f; // well behind
    lights[0].radius = 2.0f;
    lights[1].position =
        camera.position() + camera.viewDirection() * 300.0f; // past far
    lights[1].radius = 2.0f;
    ClusterLights lists;
    assignLights(s.grid, s.view, lights, lists);
    CHECK(lists.indices.empty());
  }

  TEST("a crowded cluster should keep the first lights, up to the cap")
  {
    Camera3D camera;
    camera.distance = 20.0f;
    const Setup s = setup(camera, {256.0f, 256.0f});
    std::vector<PointLight3D> lights(40);
    for (PointLight3D& light : lights)
    {
      light.position = camera.target;
      light.radius = 1.0f;
    }
    ClusterLights lists;
    assignLights(s.grid, s.view, lights, lists, 16);
    CHECK(lists.overfull > 0u);
    bool capped = true, firstOnes = true;
    for (const glm::uvec2& range : lists.ranges)
    {
      capped = capped && range.y <= 16u;
      for (uint32_t k = 0; k < range.y; ++k)
        firstOnes = firstOnes && lists.indices[range.x + k] == k;
    }
    CHECK(capped);
    CHECK(firstOnes);
  }

  return testing::report("lightClustersTests");
}
