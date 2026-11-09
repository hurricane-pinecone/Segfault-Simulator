#include "../../testHarness.h"

#include <engine/runtime/navigation/coarseGraph.h>

using namespace sfs::nav;

namespace
{
// A fence across x = 20 (y from -30 to 30) with a gap `gap` wide round
// y = `at`.
BoxSpace fence(float gap, float at = 0.0f)
{
  const float lo = at - 0.5f * gap, hi = at + 0.5f * gap;
  std::vector<BoxSpace::Solid> s;
  s.push_back({{20.0f, 0.5f * (-30.0f + lo)},
               {0.1f, 0.5f * (lo + 30.0f)},
               0.0f,
               0.0f,
               1.2f,
               false,
               1});
  s.push_back({{20.0f, 0.5f * (hi + 30.0f)},
               {0.1f, 0.5f * (30.0f - hi)},
               0.0f,
               0.0f,
               1.2f,
               false,
               2});
  return BoxSpace(s);
}
const Rect kArea{{-10.0f, -40.0f}, {50.0f, 40.0f}};

// Whether a polyline passes x = 20 within `tol` of y = `at`.
bool crossesNear(const std::vector<glm::vec2>& line, float at, float tol)
{
  for (std::size_t i = 1; i < line.size(); ++i)
  {
    const glm::vec2 a = line[i - 1], b = line[i];
    if ((a.x - 20.0f) * (b.x - 20.0f) > 0.0f || a.x == b.x)
      continue;
    const float t = (20.0f - a.x) / (b.x - a.x);
    if (std::abs(a.y + t * (b.y - a.y) - at) <= tol)
      return true;
  }
  return false;
}
} // namespace

int main()
{
  TEST("clearance should be the distance to the nearest solid")
  {
    const BoxSpace s = fence(4.0f);
    const auto g = CoarseGraph::build(s, kArea);
    CHECK(g->clearance({20.0f, 10.0f}) < 0.2f); // in the fence
    CHECK(std::abs(g->clearance({15.0f, 10.0f}) - 4.9f) < 0.3f);
    CHECK(std::abs(g->clearance({20.0f, 0.0f}) - 2.0f) < 0.3f); // gap middle
  }

  TEST("a route should go through the gate the rig fits")
  {
    const BoxSpace s = fence(3.0f, 12.0f);
    const auto g = CoarseGraph::build(s, kArea);
    std::vector<Pose2> narrows;
    const auto line = g->route({0.0f, 0.0f}, {40.0f, 0.0f}, 1.3f, &narrows);
    CHECK(line.size() >= 3u);
    CHECK(crossesNear(line, 12.0f, 0.5f));
    CHECK(line.front() == glm::vec2(0.0f, 0.0f));
    CHECK(line.back() == glm::vec2(40.0f, 0.0f));
    // (The gate is tight: a pose in its middle, facing through it.)
    bool inGate = false;
    for (const Pose2& p : narrows)
      inGate =
          inGate || (glm::length(p.position - glm::vec2(20.0f, 12.0f)) < 0.4f &&
                     std::abs(std::cos(p.heading)) > 0.6f);
    CHECK(inGate);
  }

  TEST("a rig too wide for the only gate should get no corridor")
  {
    const BoxSpace s = fence(2.0f);
    const auto g = CoarseGraph::build(s, {{-10.0f, -30.0f}, {50.0f, 30.0f}});
    CHECK(g->route({0.0f, 0.0f}, {40.0f, 0.0f}, 1.6f).empty());
    CHECK(!g->route({0.0f, 0.0f}, {40.0f, 0.0f}, 0.8f).empty());
  }

  TEST("of two gates it should take the one the rig fits")
  {
    std::vector<BoxSpace::Solid> s;
    // A fence with a narrow gap at y = 0 and a wide one at y = 20.
    s.push_back({{20.0f, -15.5f}, {0.1f, 14.5f}, 0.0f, 0.0f, 1.2f, false, 1});
    s.push_back({{20.0f, 9.25f}, {0.1f, 8.75f}, 0.0f, 0.0f, 1.2f, false, 2});
    s.push_back({{20.0f, 26.5f}, {0.1f, 3.5f}, 0.0f, 0.0f, 1.2f, false, 3});
    const BoxSpace space(s);
    const auto g =
        CoarseGraph::build(space, {{-10.0f, -30.0f}, {50.0f, 30.0f}});
    const auto wide = g->route({0.0f, 0.0f}, {40.0f, 0.0f}, 1.4f);
    CHECK(crossesNear(wide, 20.5f, 2.0f));
    const auto narrow = g->route({0.0f, 0.0f}, {40.0f, 0.0f}, 0.4f);
    CHECK(crossesNear(narrow, 0.0f, 0.6f));
  }

  TEST("an edge a game adds should draw the route along it")
  {
    const BoxSpace s(std::vector<BoxSpace::Solid>{});
    const auto g = CoarseGraph::build(s, {{-10.0f, -20.0f}, {50.0f, 20.0f}});
    const auto plain = g->route({0.0f, 0.0f}, {40.0f, 0.0f}, 1.0f);
    CHECK(plain.size() == 2u); // open ground: straight there
    g->addEdge({0.0f, 0.0f}, {20.0f, 8.0f}, 0.3f);
    g->addEdge({20.0f, 8.0f}, {40.0f, 0.0f}, 0.3f);
    const auto worn = g->route({0.0f, 0.0f}, {40.0f, 0.0f}, 1.0f);
    float far = 0.0f;
    for (const glm::vec2& p : worn)
      far = std::max(far, p.y);
    CHECK(far > 5.0f);
  }

  return testing::report("coarseGraphTests");
}
