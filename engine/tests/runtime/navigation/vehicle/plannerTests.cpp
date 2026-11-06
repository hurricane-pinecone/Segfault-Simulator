#include "../../../testHarness.h"

#include <engine/runtime/navigation/vehicle/follower.h>
#include <engine/runtime/navigation/vehicle/planner.h>
#include <engine/runtime/navigation/vehicle/speedProfile.h>
#include <engine/runtime/navigation/vehicle/steering.h>

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
  TEST("open ground should be a plain continuous-curvature path")
  {
    const PlanRequest q = request({}, {{0, 0}, 0.0f}, {{40, 12}, 0.6f});
    const PlanResult r = planNow(q);
    timed("r", r);
    CHECK(r.state == PlanState::Found);
    CHECK(r.stats.plain);
    CHECK(r.path.cusps() == 0);
    CHECK(glm::length(r.path.end().position - glm::vec2(40, 12)) < 0.03f);
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

  return testing::report("plannerTests");
}
