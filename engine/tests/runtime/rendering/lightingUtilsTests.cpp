#include "../../testHarness.h"

#include <engine/runtime/rendering/util/isometric/isometricLightingUtils.h>
#include <glm/glm/geometric.hpp>

#include <vector>

using namespace sfs;

int main()
{
  TEST("a world direction should map to a unit isometric direction")
  {
    const glm::vec2 dir =
        gridDirectionToIsometricDirection({1.0f, 0.0f}, 1.0f, 32, 16);
    CHECK(testing::approx(glm::length(dir), 1.0f));
  }

  TEST("a degenerate direction should map to zero")
  {
    const glm::vec2 dir =
        gridDirectionToIsometricDirection({0.0f, 0.0f}, 1.0f, 32, 16);
    CHECK(testing::approx(dir.x, 0.0f));
    CHECK(testing::approx(dir.y, 0.0f));
  }

  TEST("projected shadow length should scale with height and cap")
  {
    CHECK(testing::approx(projectedShadowLength(0.0f, 1.0f, 10.0f), 0.0f));
    CHECK(testing::approx(projectedShadowLength(2.0f, 1.0f, 10.0f), 2.0f));
    // length 200 capped to maxLength(1) * height(2) = 2.
    CHECK(testing::approx(projectedShadowLength(2.0f, 100.0f, 1.0f), 2.0f));
  }

  TEST("a point light within range should cast one shadow")
  {
    std::vector<IsometricPointLightSnapshot> lights(1);
    lights[0].worldPosition = {0.0f, 0.0f};
    lights[0].radius = 10.0f;
    lights[0].intensity = 1.0f;
    lights[0].height = 16.0f;

    const glm::vec3 sunStraightDown{0.0f, 0.0f, 1.0f}; // no horizontal sun cast
    const auto shadows = computeIsometricShadows(
        lights, sunStraightDown, 0.85f, {3.0f, 0.0f}, 32, 1.0f, 32, 16);

    CHECK(shadows.size() == 1);
    CHECK(shadows[0].alpha > 0.0f);
  }

  TEST("a point light beyond its radius should cast nothing")
  {
    std::vector<IsometricPointLightSnapshot> lights(1);
    lights[0].worldPosition = {0.0f, 0.0f};
    lights[0].radius = 10.0f;
    lights[0].intensity = 1.0f;

    const glm::vec3 sunStraightDown{0.0f, 0.0f, 1.0f};
    const auto shadows = computeIsometricShadows(
        lights, sunStraightDown, 0.85f, {20.0f, 0.0f}, 32, 1.0f, 32, 16);

    CHECK(shadows.empty()); // caster outside the light radius
  }

  TEST("a sun above the horizon should cast a directional shadow")
  {
    const glm::vec3 sun{0.5f, 0.0f, 0.5f}; // well above the horizon, tilted
    const auto shadows =
        computeIsometricShadows({}, sun, 0.85f, {0.0f, 0.0f}, 32, 1.0f, 32, 16);

    CHECK(shadows.size() == 1);
    CHECK(shadows[0].alpha > 0.0f);
  }

  TEST("a sun below the horizon should cast nothing")
  {
    const glm::vec3 sun{0.5f, 0.0f, -0.1f}; // under the horizon
    const auto shadows =
        computeIsometricShadows({}, sun, 0.85f, {0.0f, 0.0f}, 32, 1.0f, 32, 16);

    CHECK(shadows.empty());
  }

  return testing::report("lightingUtilsTests");
}
