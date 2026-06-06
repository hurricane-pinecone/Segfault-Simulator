// Tests for the POD keyframed Curve and Gradient samplers used by particle
// effects: clamping outside the range, interpolation, capacity, and the
// constant/linear factory helpers.

#include "../../testHarness.h"

#include <engine/core/particles/particleCurves.h>

using namespace sfs;

int main()
{
  // --- empty curve -> 0 ---------------------------------------------------
  {
    Curve c;
    CHECK(testing::approx(c.sample(0.5f), 0.0f));
  }

  // --- constant -----------------------------------------------------------
  {
    Curve c = Curve::constant(3.0f);
    CHECK(testing::approx(c.sample(0.0f), 3.0f));
    CHECK(testing::approx(c.sample(0.5f), 3.0f));
    CHECK(testing::approx(c.sample(1.0f), 3.0f));
  }

  // --- linear: endpoints, midpoint, clamping ------------------------------
  {
    Curve c = Curve::linear(0.0f, 10.0f);
    CHECK(testing::approx(c.sample(0.0f), 0.0f));
    CHECK(testing::approx(c.sample(1.0f), 10.0f));
    CHECK(testing::approx(c.sample(0.5f), 5.0f));
    CHECK(testing::approx(c.sample(-1.0f), 0.0f)); // clamp low
    CHECK(testing::approx(c.sample(2.0f), 10.0f)); // clamp high
  }

  // --- multi-stop add() ---------------------------------------------------
  {
    Curve c;
    c.add(0.0f, 0.0f).add(0.5f, 4.0f).add(1.0f, 0.0f);
    CHECK(c.count == 3);
    CHECK(testing::approx(c.sample(0.25f), 2.0f));
    CHECK(testing::approx(c.sample(0.5f), 4.0f));
    CHECK(testing::approx(c.sample(0.75f), 2.0f));
  }

  // --- add() respects MaxStops capacity -----------------------------------
  {
    Curve c;
    for (int i = 0; i < Curve::MaxStops + 4; ++i)
      c.add(static_cast<float>(i), static_cast<float>(i));
    CHECK(c.count == Curve::MaxStops);
  }

  // --- Gradient: interpolation + clamping ---------------------------------
  {
    Gradient g = Gradient::twoStop(glm::vec3{0.0f}, glm::vec3{1.0f});
    const glm::vec3 mid = g.sample(0.5f);
    CHECK(testing::approx(mid.x, 0.5f));
    CHECK(testing::approx(mid.y, 0.5f));
    CHECK(testing::approx(mid.z, 0.5f));
    CHECK(testing::approx(g.sample(-1.0f).x, 0.0f)); // clamp low
    CHECK(testing::approx(g.sample(5.0f).x, 1.0f));  // clamp high
  }

  // --- Gradient: constant -------------------------------------------------
  {
    Gradient g = Gradient::constant(glm::vec3{0.2f, 0.4f, 0.6f});
    const glm::vec3 v = g.sample(0.9f);
    CHECK(testing::approx(v.x, 0.2f));
    CHECK(testing::approx(v.y, 0.4f));
    CHECK(testing::approx(v.z, 0.6f));
  }

  return testing::report("particleCurvesTests");
}
