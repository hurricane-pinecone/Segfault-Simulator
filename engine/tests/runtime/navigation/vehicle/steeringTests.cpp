#include "../../../testHarness.h"

#include <engine/runtime/navigation/vehicle/steering.h>

#include <random>

using namespace sfs::nav;
using namespace sfs::nav::vehicle;

namespace
{
// Curvature continuous along a word except at a cusp (standing).
bool continuous(const std::vector<Segment>& w)
{
  for (std::size_t i = 1; i < w.size(); ++i)
    if (w[i].gear == w[i - 1].gear && std::abs(w[i].k0 - w[i - 1].k1) > 1e-5f)
      return false;
  return !w.empty() && std::abs(w.front().k0) < 1e-6f &&
         std::abs(w.back().k1) < 1e-6f;
}
// No segment changes curvature faster than the clothoids do.
bool sharpEnough(const std::vector<Segment>& w, float sigma)
{
  for (const Segment& s : w)
    if (s.length > 0 && std::abs(s.k1 - s.k0) / s.length > sigma * 1.001f)
      return false;
  return true;
}
} // namespace

int main()
{
  const Steering steer(1.0f / 4.6f, 0.23f);

  TEST("a CC turn should start and end on its CC circle")
  {
    std::vector<Segment> w;
    CHECK(steer.turn(1.2f, 1, 1, true, true, w));
    const Pose2 e = endOf({}, w);
    // Its circle's centre, seen from the start: (r sin mu, r cos mu).
    const glm::vec2 c{steer.ccRadius() * std::sin(steer.ccAngle()),
                      steer.ccRadius() * std::cos(steer.ccAngle())};
    CHECK(std::abs(glm::length(e.position - c) - steer.ccRadius()) < 1e-3f);
    CHECK(std::abs(wrapAngle(e.heading - 1.2f)) < 1e-4f);
    CHECK(steer.ccRadius() > 4.6f);
  }

  TEST("a turn smaller than its clothoids should still end on its CC circle")
  {
    const glm::vec2 c{steer.ccRadius() * std::sin(steer.ccAngle()),
                      steer.ccRadius() * std::cos(steer.ccAngle())};
    for (float d : {0.0f,
                    0.05f,
                    0.5f * steer.clothoidTurn(),
                    1.9f * steer.clothoidTurn()})
    {
      std::vector<Segment> w;
      CHECK(steer.turn(d, 1, 1, true, true, w));
      const Pose2 e = endOf({}, w);
      CHECK(std::abs(glm::length(e.position - c) - steer.ccRadius()) < 2e-3f);
      CHECK(std::abs(wrapAngle(e.heading - d)) < 1e-4f);
      CHECK(sharpEnough(w, steer.sigma()));
    }
  }

  TEST("every word should end exactly at the goal, curvature continuous")
  {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    int pairs = 0, withWords = 0, bad = 0, sharp = 0;
    for (int i = 0; i < 300; ++i)
    {
      const Pose2 a{{0, 0}, u(rng) * kPi};
      const Pose2 b{{25.0f * u(rng), 25.0f * u(rng)}, u(rng) * kPi};
      ++pairs;
      Steering::Words all;
      all.verify = true; // (a rare degenerate word misses: dropped)
      const auto words = steer.paths(a, b, all);
      withWords += !words.empty();
      for (const auto& w : words)
      {
        const Pose2 e = endOf(a, w);
        bad += glm::length(e.position - b.position) > 0.02f || !continuous(w);
        sharp += !sharpEnough(w, steer.sigma());
      }
    }
    CHECK(withWords == pairs);
    std::printf("  (words: %d pairs, bad %d)\n", pairs, bad);
    CHECK(bad == 0);
    CHECK(sharp == 0);
  }

  TEST("forward only should give forward words alone")
  {
    const auto words =
        steer.paths({{0, 0}, 0.0f}, {{20, 6}, 0.5f}, {true, false, false});
    CHECK(!words.empty());
    for (const auto& w : words)
      for (const Segment& s : w)
        CHECK(s.gear == 1);
  }

  TEST("turning round in a lane should find a three-point turn")
  {
    // Facing back the way it came, a metre to the side: no forward word fits
    // in less than a loop; C|C|C does it in place.
    const auto words = steer.paths({{0, 0}, 0.0f}, {{0, 1}, kPi}, {});
    float shortest = 1e9f;
    int cuspy = 0;
    for (const auto& w : words)
    {
      int cusps = 0;
      for (std::size_t i = 1; i < w.size(); ++i)
        cusps += w[i].gear != w[i - 1].gear;
      if (cusps == 2)
      {
        ++cuspy;
        shortest = std::min(shortest, lengthOf(w));
      }
    }
    CHECK(cuspy > 0);
    CHECK(shortest < 20.0f);
  }

  TEST("a path should sample to its end, the trailer swinging back straight")
  {
    VehiclePath p;
    p.start = {{0, 0}, 0};
    std::vector<Segment> w;
    steer.turn(1.5f, 1, 1, true, true, w);
    w.push_back({1, 30.0f, 0, 0});
    p.segments = w;
    Trailer t;
    sample(p, 0.1f, &t);
    CHECK(glm::length(p.points.back().pose.position -
                      endOf(p.start, w).position) < 1e-4f);
    CHECK(std::abs(p.points.back().hitch) < 0.05f);
    CHECK(std::abs(p.points.back().s - lengthOf(w)) < 1e-3f);
  }

  return testing::report("steeringTests");
}
