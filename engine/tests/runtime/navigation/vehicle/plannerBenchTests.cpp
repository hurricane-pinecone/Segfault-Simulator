#include "../../../testHarness.h"

#include <engine/runtime/navigation/vehicle/motions.h>
#include <engine/runtime/navigation/vehicle/planner.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

// The planner's benchmark: a fixed set of headland turns and yard trips (the
// kinds a farm game asks most), each planned and timed. Every way's time, cusps
// and metres backing are held to the ones the planner found before its speed
// work (kBefore): no worse. Its p50/p95 ms are printed per kind.

using namespace sfs::nav;
using namespace sfs::nav::vehicle;

namespace
{
VehicleModel tractor()
{
  VehicleModel v;
  v.boxes = {{-1.11f, 3.87f, 1.31f, 0.0f, 0.0f, 3.0f}};
  v.minRadius = 4.61f;
  v.wheelbase = 2.62f;
  v.steerRate = 1.22f;
  v.topSpeed = 8.0f;
  return v;
}
VehicleModel towing()
{
  VehicleModel v = tractor();
  Trailer t;
  t.hitchBehind = 1.0f;
  t.drawbar = 4.26f;
  t.box = {-2.2f, 3.4f, 1.24f, 0.0f, 0.3f, 2.1f};
  v.trailer = t;
  v.minRadius = 6.5f;
  return v;
}
// A tractor with a wide tool behind (a field rig).
VehicleModel rig()
{
  VehicleModel v = tractor();
  v.boxes.push_back({-4.5f, -1.0f, 1.9f, 0.0f, 0.2f, 1.5f});
  v.minRadius = 6.0f;
  return v;
}

// Standing crop: the reference point may not be over it (a field's
// unworked lanes), cut ground and the headland are ordinary.
class Crop : public CostLayer
{
public:
  Rect crop;
  explicit Crop(Rect r) : crop(r) {}
  float factor(Pose2 p, int) const override
  {
    return crop.contains(p.position) ? kInf : 1.0f;
  }
  void raster(glm::vec2 origin,
              float cell,
              int width,
              int height,
              int,
              std::vector<float>& out) const override
  {
    out.assign(static_cast<std::size_t>(width) * height, 1.0f);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x)
      {
        const glm::vec2 lo = origin + glm::vec2(x, y) * cell;
        if (crop.contains(lo) && crop.contains(lo + cell))
          out[static_cast<std::size_t>(y) * width + x] = kInf;
      }
  }
};

BoxSpace::Solid wall(glm::vec2 a, glm::vec2 b, uint64_t id, float thick = 0.15f)
{
  const glm::vec2 d = b - a;
  return {0.5f * (a + b),
          {0.5f * glm::length(d), thick},
          std::atan2(d.y, d.x),
          0.0f,
          1.4f,
          false,
          id};
}

struct Case
{
  std::string kind;
  PlanRequest q;
};

// Headland turns in a fenced field 90 x 60: lanes along x, 4 m apart, the
// crop beyond the lanes worked so far, a 10 m headland at each end, the
// fence 1 m outside the field.
std::vector<Case> turns()
{
  std::vector<Case> out;
  const Rect field{{0.0f, 0.0f}, {90.0f, 60.0f}};
  std::vector<BoxSpace::Solid> fence;
  uint64_t id = 1;
  const glm::vec2 lo = field.lo - 1.0f, hi = field.hi + 1.0f;
  fence.push_back(wall(lo, {hi.x, lo.y}, id++));
  fence.push_back(wall({hi.x, lo.y}, hi, id++));
  fence.push_back(wall(hi, {lo.x, hi.y}, id++));
  fence.push_back(wall({lo.x, hi.y}, lo, id++));
  fence.push_back({{45.0f, 30.0f}, {0.4f, 0.4f}, 0.0f, 0.0f, 3.0f, true, id++});
  const auto space = std::make_shared<BoxSpace>(fence);
  // (Each turn its own: lane spacing 3..6 m, a headland 8..14 m wide,
  // either end, the field rig or the bare tractor.)
  float y = 2.5f;
  for (int lane = 0; lane < 12; ++lane)
  {
    const float spacing = 3.0f + static_cast<float>(lane % 4);
    const float headland = 8.0f + 3.0f * static_cast<float>(lane % 3);
    const bool east = lane % 2 == 0; // (working east, turning at the east end)
    const float end = east ? field.hi.x - headland : headland;
    PlanRequest q;
    q.vehicle = (lane % 5 == 4) ? tractor() : rig();
    q.space = space;
    q.cost = std::make_shared<Crop>(Rect{
        {headland, y + spacing + 2.0f}, {field.hi.x - headland, field.hi.y}});
    q.from = {{end, y}, east ? 0.0f : kPi};
    q.goals = {{{{end, y + spacing}, east ? kPi : 0.0f}}};
    out.push_back({"turn", q});
    y += spacing;
  }
  return out;
}

// Trips about a yard: sheds, a fence with a gate, trees; from and to random
// places (a fixed seed), with and without a trailer.
std::vector<Case> trips()
{
  std::vector<Case> out;
  std::vector<BoxSpace::Solid> s;
  uint64_t id = 100;
  s.push_back({{20.0f, 20.0f}, {8.0f, 5.0f}, 0.0f, 0.0f, 5.0f, false, id++});
  s.push_back({{-15.0f, 25.0f}, {6.0f, 9.0f}, 0.3f, 0.0f, 5.0f, false, id++});
  s.push_back({{30.0f, -20.0f}, {10.0f, 4.0f}, 0.0f, 0.0f, 5.0f, false, id++});
  // (A fence along y = 0 from x = -40 to 60, a gate 7 m wide at x = 5.)
  s.push_back(wall({-40.0f, 0.0f}, {1.5f, 0.0f}, id++));
  s.push_back(wall({8.5f, 0.0f}, {60.0f, 0.0f}, id++));
  for (int t = 0; t < 6; ++t)
    s.push_back({{-30.0f + 9.0f * t, -12.0f + 3.0f * (t % 2)},
                 {0.5f, 0.5f},
                 0.0f,
                 0.0f,
                 8.0f,
                 true,
                 id++});
  const auto space = std::make_shared<BoxSpace>(s);
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> x(-35.0f, 55.0f), y(-35.0f, 40.0f),
      h(-kPi, kPi);
  int made = 0;
  while (made < 16)
  {
    PlanRequest q;
    q.vehicle = made % 2 ? towing() : tractor();
    q.space = space;
    q.from = {{x(rng), y(rng)}, h(rng)};
    const Pose2 to{{x(rng), y(rng)}, h(rng)};
    if (glm::length(to.position - q.from.position) < 15.0f ||
        touches(q, q.from, 0.0f) || touches(q, to, 0.0f))
      continue;
    q.goals = {{to}};
    out.push_back({"trip", q});
    ++made;
  }
  return out;
}

int cuspsOf(const VehiclePath& p)
{
  int n = 0, gear = 0;
  for (const Segment& s : p.segments)
  {
    if (s.length <= 0.0f)
      continue;
    if (gear != 0 && s.gear != gear)
      ++n;
    gear = s.gear;
  }
  return n;
}
float backingOf(const VehiclePath& p)
{
  float m = 0.0f;
  for (const Segment& s : p.segments)
    if (s.gear < 0)
      m += s.length;
  return m;
}
double pct(std::vector<double> v, double p)
{
  if (v.empty())
    return 0.0;
  std::sort(v.begin(), v.end());
  const double k = (static_cast<double>(v.size()) - 1.0) * p / 100.0;
  const std::size_t f = static_cast<std::size_t>(k);
  const std::size_t c = std::min(f + 1, v.size() - 1);
  return v[f] + (v[c] - v[f]) * (k - static_cast<double>(f));
}

// Before the speed work (the planner as of 2026-11-10): each case's time,
// cusps and metres backing, in order (turns, then trips); a negative time:
// no way found.
struct Before
{
  float seconds;
  int cusps;
  float backing;
};
const Before kBefore[] = {
#include "plannerBenchBefore.inc"
};
} // namespace

int main()
{
  TEST("the benchmark's ways should be no worse than before, and timed")
  {
    std::vector<Case> all = turns();
    for (Case& c : trips())
      all.push_back(std::move(c));
    std::vector<double> ms[2];
    uint64_t nodes[2] = {0, 0};
    const std::size_t known = sizeof(kBefore) / sizeof(kBefore[0]);
    int better = 0, same = 0, worse = 0;
    for (std::size_t i = 0; i < all.size(); ++i)
    {
      if (const char* o = std::getenv("A1ONLY");
          o && std::atoi(o) != static_cast<int>(i))
        continue;
      const PlanResult r = planNow(all[i].q);
      const int k = all[i].kind == "turn" ? 0 : 1;
      ms[k].push_back(r.stats.ms);
      nodes[k] += r.stats.nodes;
      const bool found = r.state == PlanState::Found;
      const float secs = found ? r.seconds : -1.0f;
      const int cusps = found ? cuspsOf(r.path) : 0;
      const float back = found ? backingOf(r.path) : 0.0f;
      // (The line kBefore is made from.)
      std::printf(
          "  {%.3ff, %d, %.2ff}, // %s %zu: %u nodes, %.1f ms [heur %.1f "
          "(heading %.1f) shot %.1f (words %.1f) near %.1f finish %.1f; "
          "checks %llu + %llu broad %.1f ms; costs %llu %.1f ms]\n",
          secs,
          cusps,
          back,
          all[i].kind.c_str(),
          i,
          r.stats.nodes,
          r.stats.ms,
          r.stats.heuristicMs,
          r.stats.headingMs,
          r.stats.shotMs,
          r.stats.wordsMs,
          r.stats.nearMs,
          r.stats.finishMs,
          static_cast<unsigned long long>(r.stats.checks),
          static_cast<unsigned long long>(r.stats.broadChecks),
          r.stats.checkMs,
          static_cast<unsigned long long>(r.stats.costCalls),
          r.stats.costMs);
      if (i >= known)
        continue;
      const Before& b = kBefore[i];
      if (b.seconds < 0.0f)
        continue; // (none before: anything found is better)
      CHECK(found);
      if (!found)
        continue;
      CHECK(secs <= b.seconds * 1.001f + 1e-3f);
      CHECK(cusps <= b.cusps);
      CHECK(back <= b.backing + 0.05f);
      if (secs < b.seconds - 0.01f)
        ++better;
      else if (secs > b.seconds + 0.01f)
        ++worse;
      else
        ++same;
    }
    std::printf(
        "  vs before: %d better, %d same, %d worse\n", better, same, worse);
    const char* names[2] = {"turns", "trips"};
    for (int k = 0; k < 2; ++k)
      std::printf("  %s: n=%zu p50 %.1f ms, p95 %.1f ms, nodes %llu\n",
                  names[k],
                  ms[k].size(),
                  pct(ms[k], 50),
                  pct(ms[k], 95),
                  static_cast<unsigned long long>(nodes[k]));
  }
  return testing::report("plannerBenchTests");
}
