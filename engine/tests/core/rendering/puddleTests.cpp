#include "../../testHarness.h"

#include <engine/core/rendering/deformation.h>
#include <engine/core/rendering/puddles.h>
#include <engine/core/rendering/terrain.h>

#include "glm/glm/geometric.hpp"

#include <cmath>
#include <vector>

using namespace sfs;

namespace
{

bool near(float a, float b, float tolerance = 1e-4f)
{
  return std::abs(a - b) <= tolerance;
}

// A flat square plane, `half` either side of the origin, as two triangles.
GroundSurface plane(float half, float z = 0.0f)
{
  const std::vector<glm::vec3> positions = {
      {-half, -half, z}, {half, -half, z}, {half, half, z}, {-half, half, z}};
  const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
  const std::vector<glm::vec2> uvs = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  return GroundSurface(positions, indices, uvs, glm::mat4(1.0f));
}

} // namespace

int main()
{
  TEST("the window should centre where the view meets the ground")
  {
    // 10 m up, looking down at 45 degrees along +x: the ground 10 m out.
    const glm::vec3 view = glm::normalize(glm::vec3(1.0f, 0.0f, -1.0f));
    const glm::vec2 center =
        puddleWindowCenter(glm::vec3(0.0f, 0.0f, 10.0f), view, 0.0f);
    CHECK(near(center.x, 10.0f) && near(center.y, 0.0f));
    // On ground 4 m up: 6 m out.
    CHECK(near(
        puddleWindowCenter(glm::vec3(0.0f, 0.0f, 10.0f), view, 4.0f).x, 6.0f));
  }

  TEST("the window should go no further out than its reach")
  {
    // Nearly level: the ground's far off.
    const glm::vec3 view = glm::normalize(glm::vec3(0.0f, 1.0f, -0.01f));
    const glm::vec2 center =
        puddleWindowCenter(glm::vec3(3.0f, 0.0f, 10.0f), view, 0.0f);
    CHECK(near(center.x, 3.0f) && near(center.y, kPuddleReach));
    // Looking up, too.
    const glm::vec3 up = glm::normalize(glm::vec3(0.0f, 1.0f, 0.5f));
    CHECK(near(puddleWindowCenter(glm::vec3(0.0f), up, 0.0f).y, kPuddleReach));
    // Straight down: under the eye.
    const glm::vec2 below = puddleWindowCenter(
        glm::vec3(2.0f, 5.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.0f);
    CHECK(near(below.x, 2.0f) && near(below.y, 5.0f));
  }

  TEST("the window should lie on whole ground texels around its centre")
  {
    const glm::vec2 origin = puddleWindowOrigin({10.3f, -7.9f});
    const glm::vec2 steps = origin / kPuddleGroundTexel;
    CHECK(near(steps.x, std::round(steps.x)) &&
          near(steps.y, std::round(steps.y)));
    const glm::vec2 middle = origin + 0.5f * kPuddleWindow;
    CHECK(glm::length(middle - glm::vec2(10.3f, -7.9f)) <
          kPuddleGroundTexel * 1.5f);
    CHECK(kPuddleGroundTexels * kPuddleGroundTexel == kPuddleWindow);
  }

  TEST("the window should move only once its centre's wandered off")
  {
    const glm::vec2 origin = puddleWindowOrigin({0.0f, 0.0f});
    CHECK(!puddleWindowMoves(origin, {0.0f, 0.0f}));
    CHECK(!puddleWindowMoves(origin, {kPuddleMove * 0.5f, 0.0f}));
    CHECK(puddleWindowMoves(origin, {0.0f, kPuddleMove * 1.5f}));
  }

  TEST("the ground under the window should be the mesh's, off it -1")
  {
    const GroundSurface ground = plane(20.0f, 0.5f);
    TerrainMaterial terrain;
    terrain.layers.push_back({});
    const glm::vec2 origin = puddleWindowOrigin({0.0f, 0.0f});
    const std::vector<glm::vec4> texels = puddleGround(ground, terrain, origin);
    CHECK(texels.size() ==
          std::size_t(kPuddleGroundTexels) * kPuddleGroundTexels);
    // The middle's on the plane: its height, pooling as its layer's.
    const std::size_t middle =
        std::size_t(kPuddleGroundTexels / 2) * kPuddleGroundTexels +
        kPuddleGroundTexels / 2;
    CHECK(near(texels[middle].x, 0.5f));
    CHECK(near(texels[middle].w, terrain.layers[0].puddles));
    // A corner's 64 m out: off it.
    CHECK(texels[0].w < 0.0f);
  }

  TEST("the ground's pooling should follow its layers' puddles")
  {
    const GroundSurface ground = plane(80.0f);
    TerrainMaterial terrain;
    terrain.layers.push_back({});
    terrain.layers[0].puddles = 0.0f;
    const std::vector<glm::vec4> meadow =
        puddleGround(ground, terrain, puddleWindowOrigin({0.0f, 0.0f}));
    bool dry = true;
    for (const glm::vec4& texel : meadow)
      dry = dry && near(texel.w, 0.0f);
    CHECK(dry);
  }

  TEST("an empty window's puddles should be empty")
  {
    TerrainPuddles puddles;
    CHECK(puddles.empty());
    puddles.size = kPuddleTexels;
    CHECK(puddles.empty()); // no ground yet
    puddles.ground = std::make_shared<const std::vector<glm::vec4>>();
    CHECK(!puddles.empty());
    CHECK(near(puddles.extent().x, kPuddleWindow));
  }

  return testing::report("puddleTests");
}
