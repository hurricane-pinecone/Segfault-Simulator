#include "../../../testHarness.h"

#include <engine/runtime/navigation/vehicle/follower.h>
#include <engine/runtime/navigation/vehicle/motions.h>
#include <engine/runtime/navigation/vehicle/planner.h>
#include <engine/runtime/navigation/vehicle/speedProfile.h>
#include <engine/runtime/navigation/vehicle/steering.h>

#include <atomic>
#include <cstdio>
#include <thread>

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
// A fence across x = 20 with a gap `gap` wide round y = 0.
std::vector<BoxSpace::Solid> fenceWithGap(float gap)
{
  std::vector<BoxSpace::Solid> s;
  uint64_t id = 1;
  for (float sign : {-1.0f, 1.0f})
    s.push_back({{20.0f, sign * (0.5f * gap + 15.0f)},
                 {0.1f, 15.0f},
                 0.0f,
                 0.0f,
                 1.2f,
                 false,
                 id++});
  return s;
}
PlanRequest request(std::vector<BoxSpace::Solid> solids, Pose2 from, Pose2 to)
{
  PlanRequest q;
  q.vehicle = tractor();
  q.space = std::make_shared<BoxSpace>(std::move(solids));
  q.from = from;
  q.goals = {{to}};
  return q;
}
// Ordinary ground that counts the rasters asked of it (the 2D heuristic's).
class CountedCost : public CostLayer
{
public:
  mutable std::atomic<int> rasters{0};
  float factor(Pose2, int) const override { return 1.0f; }
  void raster(glm::vec2 origin,
              float cell,
              int width,
              int height,
              int mode,
              std::vector<float>& out) const override
  {
    ++rasters;
    CostLayer::raster(origin, cell, width, height, mode, out);
  }
};
// Every point of the planned path clear by the exact boxes.
bool clearAll(const PlanRequest& q, const VehiclePath& p)
{
  auto probe = q.space->prepare(q.vehicle.boxes, 0.0f, 0.0f);
  for (const PathPoint& pt : p.points)
    if (!q.space->fits(probe.get(), pt.pose))
      return false;
  return true;
}
} // namespace

int main()
{
  const auto timed = [](const char* what, const PlanResult& r)
  {
    std::printf("  %s: %u nodes, %u shots, %.0f ms\n",
                what,
                r.stats.nodes,
                r.stats.shots,
                r.stats.ms);
  };
  (void)timed;
  TEST("open ground should be a continuous-curvature path, no cusps")
  {
    const PlanRequest q = request({}, {{0, 0}, 0.0f}, {{40, 12}, 0.6f});
    const PlanResult r = planNow(q);
    timed("r", r);
    CHECK(r.state == PlanState::Found);
    CHECK(r.path.cusps() == 0);
    CHECK(glm::length(r.path.end().position - glm::vec2(40, 12)) < 0.03f);
  }

  TEST("open ground should build the heuristic at most once; a searched "
       "way builds it once")
  {
    auto cost = std::make_shared<CountedCost>();
    PlanRequest q = request({}, {{0, 0}, 0.0f}, {{40, 12}, 0.6f});
    q.cost = cost;
    const PlanResult plain = planNow(q);
    CHECK(plain.state == PlanState::Found);
    CHECK(cost->rasters.load() <= 1);
    const int before = cost->rasters.load();
    PlanRequest g =
        request(fenceWithGap(2.62f + 0.6f), {{0, 6}, 0.0f}, {{40, -4}, 0.0f});
    g.cost = cost;
    const PlanResult searched = planNow(g);
    CHECK(searched.state == PlanState::Found);
    CHECK(!searched.stats.plain);
    CHECK(cost->rasters.load() == before + 1);
  }

  TEST("a gate 15 cm wider than the tractor should be threaded")
  {
    const PlanRequest q =
        request(fenceWithGap(2.62f + 0.15f), {{0, 6}, 0.0f}, {{40, -4}, 0.0f});
    const PlanResult r = planNow(q);
    timed("r", r);
    CHECK(r.state == PlanState::Found);
    CHECK(clearAll(q, r.path));
    CHECK(!r.stats.exactMiss);
  }

  TEST("a gate narrower than the tractor should be proven blocked")
  {
    // A pen 16 x 16 round the start, its one gap 2.5 m (the tractor's 2.66).
    std::vector<BoxSpace::Solid> s = {
        {{0, 8}, {8, 0.1f}, 0, 0, 1.2f, false, 1},
        {{0, -8}, {8, 0.1f}, 0, 0, 1.2f, false, 2},
        {{-8, 0}, {0.1f, 8}, 0, 0, 1.2f, false, 3},
        {{8, 1.25f + 3.375f}, {0.1f, 3.375f}, 0, 0, 1.2f, false, 4},
        {{8, -1.25f - 3.375f}, {0.1f, 3.375f}, 0, 0, 1.2f, false, 5}};
    const PlanRequest q = request(s, {{0, 0}, 0.0f}, {{30, 0}, 0.0f});
    const PlanResult r = planNow(q);
    timed("r", r);
    std::printf("blocked: %u nodes %.0f ms\n", r.stats.nodes, r.stats.ms);
    CHECK(r.state == PlanState::Blocked);
    CHECK(!r.blockers.empty());
  }

  TEST("a goal something stands on should be proven blocked at once")
  {
    // A parked truck's corner over the goal, open ground everywhere else: no
    // search over everything reachable, what's on the goal as the blocker.
    const std::vector<BoxSpace::Solid> truck = {
        {{41.0f, 2.5f}, {3.8f, 1.4f}, 0.0f, 0.0f, 4.0f, false, 7}};
    const PlanRequest q = request(truck, {{0, 0}, 0.0f}, {{40, 0}, 0.0f});
    const PlanResult r = planNow(q);
    timed("r", r);
    CHECK(r.state == PlanState::Blocked);
    CHECK(r.stats.nodes == 0u);
    CHECK(r.blockers.size() == 1u);
    CHECK(!r.blockers.empty() && r.blockers.front().id == 7u);
    // (Moved off it, the same goal's found.)
    const PlanRequest clear =
        request({{{41.0f, 4.0f}, {3.8f, 1.4f}, 0.0f, 0.0f, 4.0f, false, 7}},
                {{0, 0}, 0.0f},
                {{40, 0}, 0.0f});
    CHECK(planNow(clear).state == PlanState::Found);
    // (Starting touching it, within getting out: not refused.)
    const PlanRequest escape =
        request(truck, {{40, 0.3f}, 0.0f}, {{40, 0}, 0.0f});
    const PlanResult e = planNow(escape);
    CHECK(e.why.find("where it's to stop") == std::string::npos);
  }

  TEST("touches() should be the planner's own goal check, gap included")
  {
    // The tractor's left side at y 1.31; a wall's face 1 cm past it (inside
    // the 2 cm gap), then 5 cm past it.
    const auto wallAt = [](float face)
    {
      return std::vector<BoxSpace::Solid>{
          {{40.0f, face + 0.5f}, {6.0f, 0.5f}, 0.0f, 0.0f, 2.0f, false, 9}};
    };
    const Pose2 goal{{40, 0}, 0.0f};
    const PlanRequest near = request(wallAt(1.32f), {{0, 0}, 0.0f}, goal);
    const std::optional<Space::Hit> hit = touches(near, goal, std::nullopt);
    CHECK(hit.has_value() && hit->id == 9u);
    CHECK(planNow(near).state == PlanState::Blocked);
    const PlanRequest off = request(wallAt(1.36f), {{0, 0}, 0.0f}, goal);
    CHECK(!touches(off, goal, std::nullopt).has_value());
    CHECK(planNow(off).state == PlanState::Found);
  }

  TEST("touchesAlong() should say where a game's own way first touches")
  {
    // A post 1 m across at x = 20 on the line; the tractor's nose 3.87 m
    // ahead of its reference point (and the 2 cm gap).
    const std::vector<BoxSpace::Solid> post = {
        {{20.0f, 0.0f}, {0.5f, 0.5f}, 0.0f, 0.0f, 2.0f, false, 7}};
    const PlanRequest q = request(post, {{0, 0}, 0.0f}, {{40, 0}, 0.0f});
    const std::vector<Segment> through{{1, 30.0f, 0.0f, 0.0f}};
    const std::optional<float> at =
        touchesAlong(q, {{0, 0}, 0.0f}, 0.0f, through);
    CHECK(at.has_value());
    if (at)
      CHECK(std::abs(*at - (19.5f - 3.87f - 0.02f)) < 0.15f);
    // Beside it, 3 m off: clear.
    CHECK(!touchesAlong(q, {{0, 3.0f}, 0.0f}, 0.0f, through).has_value());
    // Backing up through it: touches too (either gear checked).
    const std::vector<Segment> back{{-1, 30.0f, 0.0f, 0.0f}};
    CHECK(touchesAlong(q, {{40, 0}, 0.0f}, 0.0f, back).has_value());
    // Starting touching it, getting out within kEscapeReach: let off.
    const std::vector<Segment> out{{-1, 4.0f, 0.0f, 0.0f}};
    CHECK(!touchesAlong(q, {{17.0f, 0}, 0.0f}, 0.0f, out).has_value());
    // Ending against it (a hitch): let off only with endIn.
    const std::vector<Segment> onto{{1, 15.8f, 0.0f, 0.0f}};
    CHECK(touchesAlong(q, {{0, 0}, 0.0f}, 0.0f, onto).has_value());
    CHECK(!touchesAlong(q, {{0, 0}, 0.0f}, 0.0f, onto, true).has_value());
    // A curve's poses, sampled as a game lays them: the same answer.
    const std::vector<Segment> arc{{1, 10.0f, 0.2f, 0.2f}};
    CHECK(!touchesAlong(q, {{0, 0}, 0.0f}, 0.0f, arc).has_value());
  }

  TEST("turning round in a dead-end lane should back up, not fail")
  {
    // A lane 6 m wide, closed at x = 30: in facing +x, out facing -x.
    std::vector<BoxSpace::Solid> s = {
        {{15, 3.0f + 0.5f}, {20, 0.5f}, 0, 0, 2, false, 1},
        {{15, -3.0f - 0.5f}, {20, 0.5f}, 0, 0, 2, false, 2},
        {{30.5f, 0}, {0.5f, 4}, 0, 0, 2, false, 3}};
    PlanRequest q = request(s, {{22, 0}, 0.0f}, {{15, 0}, kPi});
    const PlanResult r = planNow(q);
    timed("r", r);
    CHECK(r.state == PlanState::Found);
    CHECK(r.path.cusps() >= 1);
    CHECK(clearAll(q, r.path));
  }

  TEST("a short reposition should take the quickest way: backing up, not "
       "a ring")
  {
    // (A field's lap corner (a fence 4 m ahead and 3 m to the right; the
    // next leg starts 2.5 m off, turned 60 degrees left), a pass starting
    // half a metre behind where the last ended, and a lane change of half a
    // metre (the b8 field's ringing legs). Nothing caps the turning: the way
    // found is the quickest by its speed profile, at least as quick as any
    // clear analytic word there, and none of those is a loop.)
    std::vector<BoxSpace::Solid> s = {
        {{4.5f, 0}, {0.1f, 20}, 0, 0, 1.2f, false, 1},
        {{0, -4.5f}, {20, 0.1f}, 0, 0, 1.2f, false, 2}};
    const auto mostTurned = [](const VehiclePath& p)
    {
      float most = 0.0f, left = 0.0f, right = 0.0f;
      for (std::size_t k = 1; k < p.points.size(); ++k)
      {
        if (p.points[k].gear != p.points[k - 1].gear)
          left = right = 0.0f;
        const float d =
            wrapAngle(p.points[k].pose.heading - p.points[k - 1].pose.heading);
        left = std::max(left + d, 0.0f);
        right = std::max(right - d, 0.0f);
        most = std::max(most, std::max(left, right));
      }
      return most;
    };
    for (const Pose2 to : {Pose2{{-1.5f, 2.0f}, 1.05f},
                           Pose2{{-0.5f, 0.1f}, -0.17f},
                           Pose2{{-5.5f, 0.5f}, 0.0f}})
    {
      const PlanRequest q = request(s, {{-6.0f, 0.0f}, 0.0f}, to);
      const PlanResult r = planNow(q);
      timed("r", r);
      CHECK(r.state == PlanState::Found);
      CHECK(clearAll(q, r.path));
      // (Every clear word from the start to the goal, by its own profile.)
      const Motions rig(q);
      float quickest = 1e9f, loop = 1e9f;
      for (const auto& word : rig.words(q.from, to, true, 10))
      {
        Motions::Walk w;
        if (!rig.walk(q.from, word, q.tunables.exactStep, true, w) ||
            glm::length(w.end.position - to.position) > 0.1f)
          continue;
        VehiclePath p;
        p.start = q.from;
        p.segments = word;
        sample(p, 0.05f, nullptr);
        profile(p, q.vehicle);
        quickest = std::min(quickest, p.duration());
        if (mostTurned(p) >= kPi * 5.0f / 3.0f)
          loop = std::min(loop, p.duration());
      }
      std::printf("    (%.1f s found, turned %.0f deg; quickest word %.1f s, "
                  "quickest loop word %.1f s)\n",
                  r.seconds,
                  mostTurned(r.path) * 180.0f / kPi,
                  quickest,
                  loop);
      // (Within the search's pricing of a way against its profile's.)
      CHECK(r.seconds <= quickest * 1.03f);
      CHECK(r.seconds < loop);
    }
  }

  TEST("the same request should give the same path, whatever else runs")
  {
    const PlanRequest q =
        request(fenceWithGap(3.0f), {{0, 6}, 0.3f}, {{40, -4}, -0.2f});
    const PlanResult a = planNow(q);
    timed("a", a);
    std::atomic<bool> stop = false;
    std::vector<std::thread> busy;
    for (int i = 0; i < 4; ++i)
      busy.emplace_back(
          [&]
          {
            volatile double x = 0;
            while (!stop)
              x = x + 1.0;
          });
    const PlanResult b = planNow(q);
    stop = true;
    for (auto& t : busy)
      t.join();
    CHECK(a.path.points.size() == b.path.points.size());
    bool same = a.path.points.size() == b.path.points.size();
    for (std::size_t i = 0; same && i < a.path.points.size(); ++i)
      same = a.path.points[i].pose.position == b.path.points[i].pose.position;
    CHECK(same);
    CHECK(a.stats.nodes == b.stats.nodes);
  }

  TEST("a search split into small budgets should end as one big one")
  {
    PlanRequest q =
        request(fenceWithGap(2.8f), {{0, 8}, 1.0f}, {{40, -6}, 0.0f});
    q.nodeBudget = 1u << 20;
    const PlanResult whole = planNow(q);
    timed("whole", whole);
    q.nodeBudget = 64;
    int partials = 0;
    const PlanResult split = planNow(q, [&](const PlanResult&) { ++partials; });
    CHECK(whole.state == PlanState::Found);
    CHECK(split.state == PlanState::Found);
    CHECK(partials > 0);
    CHECK(std::abs(whole.seconds - split.seconds) < 1e-3f);
  }

  TEST("a trailer should be backed into a bay within its reversing limit")
  {
    VehicleModel v = tractor();
    Trailer t;
    t.hitchBehind = 1.0f;
    t.drawbar = 4.26f;
    t.box = {-2.2f, 3.4f, 1.24f, 0.0f, 0.3f, 2.1f};
    v.trailer = t;
    v.minRadius = 6.5f;
    std::vector<BoxSpace::Solid> s = {
        {{0, 4.2f}, {6, 1.0f}, 0, 0, 3, false, 1},
        {{0, -4.2f}, {6, 1.0f}, 0, 0, 3, false, 2}};
    PlanRequest q;
    q.vehicle = v;
    q.space = std::make_shared<BoxSpace>(s);
    q.from = {{-20, 14}, 0.0f};
    q.goals = {{{{2, 0}, kPi}, 0.0f, 0.12f}};
    const PlanResult r = planNow(q);
    timed("r", r);
    CHECK(r.state == PlanState::Found);
    float worst = 0;
    for (const PathPoint& p : r.path.points)
      if (p.gear < 0)
        worst = std::max(worst, std::abs(p.hitch));
    CHECK(worst <= t.reverseLimit + 0.02f);
  }

  // (A2t: a goal may say how it's come to (Goal::arriveGear): a parking spot
  // a rig is reversed into.)
  TEST("a towing rig told to arrive backing should reverse into a drive-"
       "through bay it would otherwise drive straight into")
  {
    VehicleModel v = tractor();
    Trailer t;
    t.hitchBehind = 1.0f;
    t.drawbar = 4.26f;
    t.box = {-2.2f, 3.4f, 1.24f, 0.0f, 0.3f, 2.1f};
    v.trailer = t;
    v.minRadius = 6.5f;
    // (A bay 6 m wide between two sheds, open at both ends.)
    std::vector<BoxSpace::Solid> s = {
        {{0, 5.0f}, {8, 2.0f}, 0, 0, 3, false, 1},
        {{0, -5.0f}, {8, 2.0f}, 0, 0, 3, false, 2}};
    PlanRequest q;
    q.vehicle = v;
    q.space = std::make_shared<BoxSpace>(s);
    q.from = {{-30, 0}, 0.0f};
    q.goals = {{{{2, 0}, 0.0f}, 0.0f, 0.12f}};
    const PlanResult in = planNow(q);
    CHECK(in.state == PlanState::Found);
    CHECK(in.path.cusps() == 0);
    CHECK(lastGear(in.path.segments) > 0);
    q.goals[0].arriveGear = -1;
    const PlanResult r = planNow(q);
    timed("backing in", r);
    CHECK(r.state == PlanState::Found);
    CHECK(lastGear(r.path.segments) < 0);
    CHECK(!r.path.points.empty() && r.path.points.back().gear < 0);
    // (Backed in over the whole rig's length (3.87 m nose, 7.46 m to the
    // trailer's tail), not driven in and shuffled back at the end.)
    CHECK(lastRun(r.path.segments) >= 11.3f);
    CHECK(glm::length(r.path.end().position - glm::vec2(2, 0)) < 0.03f);
    CHECK(std::abs(wrapAngle(r.path.end().heading)) < 0.01f);
    CHECK(std::abs(r.path.points.back().hitch) <= 0.12f + 0.02f);
    CHECK(!r.stats.exactMiss);
    CHECK(r.stats.nodes < 20000);
    float worst = 0;
    for (const PathPoint& p : r.path.points)
      if (p.gear < 0)
        worst = std::max(worst, std::abs(p.hitch));
    CHECK(worst <= t.reverseLimit + 0.02f);
    // (Its speed profile stands at the end, backing.)
    CHECK(r.path.points.back().speed == 0.0f);
    // (Followed, it gets there.)
    Follower f;
    f.setPath(r.path);
    for (int i = 0; i < 20000 && !f.step(0.05f).done; ++i)
    {
    }
    CHECK(glm::length(f.pose().position - glm::vec2(2, 0)) < 0.05f);
    std::printf("    (backing in: %u nodes, %.0f ms, %.1f m, %d cusps)\n",
                r.stats.nodes,
                r.stats.ms,
                r.path.length(),
                r.path.cusps());
  }

  TEST("the speed profile should stop at cusps and slow for the tight curve")
  {
    const PlanRequest q = request({}, {{0, 0}, 0.0f}, {{0, 2}, kPi});
    const PlanResult r = planNow(q);
    timed("r", r);
    CHECK(r.state == PlanState::Found);
    bool ok = true;
    for (std::size_t i = 1; i < r.path.points.size(); ++i)
    {
      const PathPoint& p = r.path.points[i];
      if (p.gear != r.path.points[i - 1].gear)
        ok = ok && p.speed == 0.0f;
      if (std::abs(p.curvature) > 1e-3f)
        ok = ok && p.speed <= std::sqrt(q.vehicle.lateralAccel /
                                        std::abs(p.curvature)) +
                                  1e-3f;
    }
    CHECK(ok);
    CHECK(r.path.points.back().speed == 0.0f);
  }

  TEST("following should drive the path exactly to its end")
  {
    const PlanRequest q = request({}, {{0, 0}, 0.0f}, {{30, 10}, 1.0f});
    const PlanResult r = planNow(q);
    timed("r", r);
    Follower f;
    f.setPath(r.path);
    int frames = 0;
    while (!f.step(1.0f / 60.0f).done && frames < 60 * 120)
      ++frames;
    CHECK(glm::length(f.pose().position - glm::vec2(30, 10)) < 0.01f);
    CHECK(frames < 60 * 60);
  }

  TEST("the stopping distance should be the one the follower stands by: "
       "braking from speed v to a stop named that far ahead, it stands there "
       "without a hard stop")
  {
    // (v * dt + v^2 / 2b, and stopSpeed its inverse.)
    CHECK(std::abs(Follower::stoppingDistance(4.0f, 2.0f) - 4.0f) < 1.0e-5f);
    CHECK(std::abs(Follower::stoppingDistance(-4.0f, 2.0f, 0.5f) - 6.0f) <
          1.0e-5f);
    CHECK(
        std::abs(Follower::stopSpeed(
                     Follower::stoppingDistance(3.0f, 1.5f, 0.1f), 1.5f, 0.1f) -
                 3.0f) < 1.0e-4f);
    const PlanRequest q = request({}, {{0, 0}, 0.0f}, {{80, 0}, 0.0f});
    const PlanResult r = planNow(q);
    CHECK(r.state == PlanState::Found);
    const float dt = 1.0f / 60.0f, brake = q.vehicle.brake;
    Follower f;
    f.setDrive({q.vehicle.shuttleSeconds, q.vehicle.accel, brake});
    f.setPath(r.path);
    // (Up to speed, then a stop named its stopping distance ahead.)
    while (f.speed() < 0.5f * q.vehicle.topSpeed && f.s() < 40.0f)
      f.step(dt);
    const float v = f.speed();
    const float stopAt = f.s() + Follower::stoppingDistance(v, brake, dt);
    bool hard = false, stood = false;
    for (int i = 0; i < 60 * 60 && !stood; ++i)
    {
      const Follower::State st =
          f.step(dt, std::numeric_limits<float>::infinity(), stopAt);
      hard = hard || st.hardStopFrom > 0.0f;
      stood = f.speed() == 0.0f;
    }
    CHECK(v > 1.0f);
    CHECK(stood && !hard);
    CHECK(std::abs(f.s() - stopAt) < 0.05f);
  }

  TEST("the follower should brake at its derived rate, the one its speed "
       "profile is priced with, and tell a stop named too late")
  {
    const PlanRequest q = request({}, {{0, 0}, 0.0f}, {{60, 0}, 0.0f});
    const PlanResult r = planNow(q);
    CHECK(r.state == PlanState::Found);
    // (Its drive's braking the rate its profile was priced with.)
    const float brake = q.vehicle.brake, dt = 1.0f / 60.0f;
    const float kInf = std::numeric_limits<float>::infinity();
    const auto drive =
        [&](float stopAt, float fromS, float* hardest, float* fastestBraking)
    {
      Follower f;
      f.setDrive({0.5f, q.vehicle.accel, brake});
      f.setPath(r.path);
      *hardest = 0.0f;
      *fastestBraking = 0.0f;
      float last = 0.0f;
      for (int k = 0; k < 60 * 120; ++k)
      {
        const Follower::State st =
            f.step(dt,
                   std::numeric_limits<float>::infinity(),
                   f.s() >= fromS ? stopAt : kInf);
        *hardest = std::max(*hardest, st.hardStopFrom);
        const float v = std::abs(f.speed());
        if (v > 0.0f)
          *fastestBraking = std::max(*fastestBraking, (last - v) / dt);
        last = v;
        if (st.done || (f.s() >= fromS && v == 0.0f && f.s() > 0.0f))
          break;
      }
      return f.s();
    };
    float hardest = 0.0f, braking = 0.0f;
    // Its own profile's stop (the end): braked for, never harder than its rate.
    drive(kInf, 0.0f, &hardest, &braking);
    CHECK(hardest == 0.0f);
    CHECK(braking <= brake + 1e-3f);
    // A stop named well ahead: braked for at its rate, stood on it.
    const float stood = drive(30.0f, 5.0f, &hardest, &braking);
    CHECK(std::abs(stood - 30.0f) < 1e-3f);
    CHECK(hardest == 0.0f);
    CHECK(braking <= brake + 1e-3f);
    // Named nearer than its brakes reach once going: never past it, told.
    const float cut = drive(10.3f, 10.0f, &hardest, &braking);
    CHECK(cut <= 10.3f + 1e-4f);
    CHECK(hardest > Follower::kCreep);
  }

  TEST("a cusp should be priced at its real time: the shuttle standing plus "
       "the time lost braking and pulling away")
  {
    VehicleModel m = tractor();
    m.shuttleSeconds = 0.5f;
    m.accel = 1.0f;
    m.brake = 2.0f;
    m.reverseSpeed = 2.0f;
    // (Both sides at 2 m/s: 0.5 + 2/(2*2) + 2/(2*1) = 2 s.)
    CHECK(std::abs(m.cuspSeconds(2.0f) - 2.0f) < 1e-4f);
    // Gentler brakes and pull-away: a dearer cusp; no shuttle: only the lost
    // time.
    VehicleModel soft = m;
    soft.accel = 0.5f;
    soft.brake = 0.5f;
    CHECK(soft.cuspSeconds(2.0f) > m.cuspSeconds(2.0f));
    m.shuttleSeconds = 0.0f;
    CHECK(std::abs(m.cuspSeconds(2.0f) - 1.5f) < 1e-4f);
  }

  TEST("the speed profile's time should stand each cusp its shuttle time")
  {
    const PlanRequest q = request({}, {{0, 0}, 0.0f}, {{0, 2}, kPi});
    const PlanResult r = planNow(q);
    CHECK(r.state == PlanState::Found);
    int cusps = 0;
    for (std::size_t i = 1; i < r.path.points.size(); ++i)
      cusps += r.path.points[i].gear != r.path.points[i - 1].gear ? 1 : 0;
    CHECK(cusps > 0);
    VehiclePath none = r.path, two = r.path;
    VehicleModel m = q.vehicle;
    m.shuttleSeconds = 0.0f;
    profile(none, m);
    m.shuttleSeconds = 2.0f;
    profile(two, m);
    CHECK(std::abs(two.duration() - none.duration() - 2.0f * cusps) < 1e-3f);
  }

  TEST("a cusp should stand it its shuttle time, braking and pulling away at "
       "its own derived rates")
  {
    const PlanRequest q = request({}, {{0, 0}, 0.0f}, {{0, 2}, kPi});
    const PlanResult r = planNow(q);
    CHECK(r.state == PlanState::Found);
    int cusps = 0;
    for (std::size_t i = 1; i < r.path.points.size(); ++i)
      cusps += r.path.points[i].gear != r.path.points[i - 1].gear ? 1 : 0;
    CHECK(cusps > 0);
    const auto standing = [&](float accel, float brake, float* seconds)
    {
      Follower f;
      f.setDrive({0.5f, accel, brake});
      f.setPath(r.path);
      const float dt = 1.0f / 60.0f;
      float waited = 0.0f, t = 0.0f, fastest = 0.0f, last = 0.0f;
      for (int k = 0; k < 60 * 120; ++k)
      {
        const Follower::State st = f.step(dt);
        t += dt;
        waited += st.waiting ? dt : 0.0f;
        const float v = std::abs(f.speed());
        fastest = std::max(fastest, (v - last) / dt);
        last = v;
        if (st.done)
          break;
      }
      *seconds = t;
      return std::pair{waited, fastest};
    };
    float slowT = 0.0f, quickT = 0.0f;
    const auto [waitedSlow, accelSlow] = standing(0.3f, 0.6f, &slowT);
    const auto [waitedQuick, accelQuick] = standing(2.0f, 3.0f, &quickT);
    // (Each cusp stands 0.5 s, plus the frame it lands in.)
    CHECK(std::abs(waitedSlow - 0.5f * cusps) < (1.0f / 60.0f) * (cusps + 1));
    CHECK(accelSlow <= 0.3f + 1e-3f);
    CHECK(accelQuick > 0.3f + 1e-3f);
    CHECK(slowT > quickT);
  }

  TEST("the background planner should hand back the same answer")
  {
    const PlanRequest q =
        request(fenceWithGap(3.0f), {{0, 6}, 0.3f}, {{40, -4}, -0.2f});
    const PlanResult now = planNow(q);
    Planner planner(2);
    const auto t = planner.submit(q);
    PlanResult r;
    for (int i = 0; i < 2000; ++i)
    {
      r = planner.poll(t);
      if (r.state == PlanState::Found || r.state == PlanState::Blocked)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(r.state == PlanState::Found);
    CHECK(std::abs(r.seconds - now.seconds) < 1e-4f);
  }

  TEST("a goal in a bay open only ahead of it should be backed into from its "
       "lined-up pose, found within a small search")
  {
    // A shed bay 4 m wide, 8 m deep, open to the east; the goal facing east
    // in it (backed in), the tractor coming from the south facing north.
    std::vector<BoxSpace::Solid> bay = {
        {{-4.0f, 2.1f}, {4.1f, 0.1f}, 0.0f, 0.0f, 3.0f, false, 1},
        {{-4.0f, -2.1f}, {4.1f, 0.1f}, 0.0f, 0.0f, 3.0f, false, 2},
        {{-8.2f, 0.0f}, {0.1f, 2.2f}, 0.0f, 0.0f, 3.0f, false, 3}};
    PlanRequest q = request(bay, {{14, -18}, 1.5708f}, {{-3.8f, 0.0f}, 0.0f});
    const PlanResult r = planNow(q);
    CHECK(r.state == PlanState::Found);
    CHECK(clearAll(q, r.path));
    CHECK(!r.path.points.empty() && r.path.points.back().gear < 0);
    CHECK(r.stats.nodes < 20000);
    std::printf(
        "    (bay: %u nodes, %u shots)\n", r.stats.nodes, r.stats.shots);
  }

  TEST("a way into a bay should never go round a loop to line up, from any "
       "side")
  {
    // The bay above, open to the east; approached from all round it, close
    // (where the lined-up approach is shot): no way turns a full circle.
    std::vector<BoxSpace::Solid> bay = {
        {{-4.0f, 2.1f}, {4.1f, 0.1f}, 0.0f, 0.0f, 3.0f, false, 1},
        {{-4.0f, -2.1f}, {4.1f, 0.1f}, 0.0f, 0.0f, 3.0f, false, 2},
        {{-8.2f, 0.0f}, {0.1f, 2.2f}, 0.0f, 0.0f, 3.0f, false, 3}};
    int found = 0;
    float most = 0.0f;
    for (const Pose2 from : {Pose2{{12, 6}, 3.1416f},
                             Pose2{{12, -6}, 3.1416f},
                             Pose2{{10, 10}, -1.5708f},
                             Pose2{{16, 0}, 1.5708f},
                             Pose2{{8, -10}, 0.0f}})
    {
      const PlanRequest q = request(bay, from, {{-3.8f, 0.0f}, 0.0f});
      const PlanResult r = planNow(q);
      if (r.state != PlanState::Found)
        continue;
      ++found;
      float turned = 0.0f;
      for (std::size_t i = 1; i < r.path.points.size(); ++i)
        turned += std::abs(r.path.points[i].curvature) *
                  (r.path.points[i].s - r.path.points[i - 1].s);
      most = std::max(most, turned);
    }
    CHECK(found == 5);
    CHECK(most < 2.0f * 3.14159265f);
    std::printf("    (most turned %.0f deg)\n", most * 57.2958f);
  }

  // The search's flood (A2r): goals that face back, or are reached only by
  // going round, priced by the heading-aware heuristic (its motions with
  // obstacles), the search grows few poses.
  TEST("a towing rig whose goal faces back behind buildings should find its "
       "way round them within a small search")
  {
    // (The cart meet: a forward-only tractor and trailer heading west, the
    // goal 60 m west facing east; buildings north of the goal's lane, a
    // fence 9 m south of it: no room to turn round there.)
    std::vector<BoxSpace::Solid> yard = {
        {{-12.0f, -1.0f}, {6.0f, 9.0f}, 0.0f, 0.0f, 4.0f, false, 1},
        {{5.0f, -19.0f}, {45.0f, 0.2f}, 0.0f, 0.0f, 1.2f, false, 2}};
    PlanRequest q =
        request(yard, {{45.0f, -10.5f}, 3.14159265f}, {{-16.0f, -14.3f}, 0.0f});
    q.vehicle.minRadius = 4.57f;
    q.vehicle.topSpeed = 6.0f;
    q.vehicle.mayReverse = false;
    Trailer t;
    t.hitchBehind = 1.0f;
    t.drawbar = 4.26f;
    t.box = {-2.04f, 4.39f, 1.235f, 0.0f, 0.0f, 3.0f};
    t.forwardLimit = 1.43f;
    q.vehicle.trailer = t;
    const PlanResult r = planNow(q);
    CHECK(r.state == PlanState::Found);
    CHECK(r.path.cusps() == 0);
    CHECK(r.stats.nodes < 20000);
    std::printf("    (round the buildings: %u nodes, %.0f ms, %.1f m)\n",
                r.stats.nodes,
                r.stats.ms,
                r.path.length());
  }

  // (A2s: the heading-aware heuristic turns as the search's own turns do (in
  // and out of lock, not held at full lock), checks the rig's outline (its
  // corners) and the trailer's, swung as a turn leaves it.)
  TEST("a towing rig turning round in a yard too narrow for its turns should "
       "go round where there's room within a small search")
  {
    // (Tractor 05 to the field: heading west in a lane 10 m wide between a
    // shed (north) and a thin fence (south); the goal 26 m east facing east:
    // a U-turn there looks as if it fits at full lock, not as it's driven.)
    std::vector<BoxSpace::Solid> yard = {
        {{-30.0f, 10.0f}, {14.0f, 6.0f}, 0.0f, 0.0f, 4.0f, false, 1},
        {{-20.0f, -6.05f}, {34.0f, 0.05f}, 0.0f, 0.0f, 1.2f, false, 2}};
    PlanRequest q =
        request(yard, {{-26.8f, 0.0f}, 3.14159265f}, {{-0.8f, 0.0f}, 0.0f});
    q.vehicle.minRadius = 4.57f;
    q.vehicle.topSpeed = 6.0f;
    q.vehicle.mayReverse = false;
    Trailer t;
    t.hitchBehind = 1.0f;
    t.drawbar = 4.26f;
    t.box = {-2.04f, 4.39f, 1.235f, 0.0f, 0.0f, 3.0f};
    t.forwardLimit = 1.43f;
    q.vehicle.trailer = t;
    const PlanResult r = planNow(q);
    CHECK(r.state == PlanState::Found);
    CHECK(r.path.cusps() == 0);
    CHECK(r.stats.nodes < 20000);
    std::printf("    (turning round: %u nodes, %.0f ms, %.1f m)\n",
                r.stats.nodes,
                r.stats.ms,
                r.path.length());
  }

  TEST("a goal in a shed facing away from the start should be found within a "
       "small search")
  {
    // (The combine home: a shed open to the west, the goal nose in facing
    // east; the start 50 m south facing east, a wall between with one gap.)
    std::vector<BoxSpace::Solid> shed = {
        {{10.0f, 31.5f}, {5.0f, 0.2f}, 0.0f, 0.0f, 4.0f, false, 1},
        {{10.0f, 25.0f}, {5.0f, 0.2f}, 0.0f, 0.0f, 4.0f, false, 2},
        {{15.2f, 28.25f}, {0.2f, 3.45f}, 0.0f, 0.0f, 4.0f, false, 3},
        {{-20.0f, 0.0f}, {20.0f, 0.3f}, 0.0f, 0.0f, 2.0f, false, 4},
        {{28.0f, 0.0f}, {20.0f, 0.3f}, 0.0f, 0.0f, 2.0f, false, 5}};
    PlanRequest q =
        request(shed, {{-14.0f, -21.0f}, 0.0f}, {{7.5f, 28.25f}, 0.0f});
    q.vehicle.minRadius = 5.28f;
    q.vehicle.boxes = {{-1.5f, 6.0f, 1.6f, 0.0f, 0.0f, 3.5f}};
    const PlanResult r = planNow(q);
    CHECK(r.state == PlanState::Found);
    CHECK(clearAll(q, r.path));
    CHECK(r.stats.nodes < 20000);
    std::printf("    (into the shed: %u nodes, %.0f ms, %.1f m, %d cusps)\n",
                r.stats.nodes,
                r.stats.ms,
                r.path.length(),
                r.path.cusps());
  }

  TEST("a way round a wall whose end is beyond the first area searched should "
       "be found after widening it, within a small search")
  {
    // (A wall 140 m long between start and goal, its end 70 m off: the
    // heading-aware heuristic finds no way in the first area and runs into
    // its edge, so the area's widened at once, not flooded.)
    std::vector<BoxSpace::Solid> wall = {
        {{0.0f, 15.0f}, {70.0f, 0.3f}, 0.0f, 0.0f, 2.0f, false, 1}};
    const PlanRequest q =
        request(wall, {{0.0f, 0.0f}, 1.5708f}, {{0.0f, 30.0f}, 1.5708f});
    const PlanResult r = planNow(q);
    CHECK(r.state == PlanState::Found);
    CHECK(r.stats.widened >= 1);
    CHECK(r.stats.nodes < 20000);
    std::printf("    (round the wall: %u nodes, %.0f ms, %.1f m, %d widened)\n",
                r.stats.nodes,
                r.stats.ms,
                r.path.length(),
                r.stats.widened);
  }

  TEST("once a shot has found a way, a quicker one is looked for only so "
       "long (Tunables::improveNodes), never the whole budget")
  {
    // (A long trip round a block: a shot finds a way early; the search may
    // look for a quicker one, then takes the best it has.)
    std::vector<BoxSpace::Solid> block = {
        {{30.0f, 0.0f}, {8.0f, 8.0f}, 0.0f, 0.0f, 2.0f, false, 1}};
    PlanRequest q = request(block, {{0.0f, 0.0f}, 0.0f}, {{70.0f, 0.0f}, 0.0f});
    q.nodeBudget = 1000000;
    q.budgetCap = 1000000;
    const PlanResult r = planNow(q);
    CHECK(r.state == PlanState::Found);
    CHECK(r.stats.nodes < 20000);
    CHECK(clearAll(q, r.path));
    std::printf("    (round the block: %u nodes, %.0f ms, %.1f m)\n",
                r.stats.nodes,
                r.stats.ms,
                r.path.length());
  }

  return testing::report("plannerTests");
}
