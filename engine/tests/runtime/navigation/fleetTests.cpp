#include "../../testHarness.h"

#include <engine/runtime/navigation/fleet.h>
#include <engine/runtime/navigation/vehicle/speedProfile.h>
#include <engine/runtime/navigation/vehicle/vehicleKind.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

using namespace sfs::nav;

namespace
{
// A car: 4.6 x 1.9 m, its rear axle 1 m from its back.
AgentSpec car()
{
  AgentSpec s;
  s.boxes = {{-1.0f, 3.6f, 0.95f, 0.0f, 0.0f, 1.6f}};
  s.minRadius = 5.0f;
  s.wheelbase = 2.7f;
  s.steerRate = 1.4f;
  s.topSpeed = 6.0f;
  return s;
}

BoxSpace::Solid wall(glm::vec2 lo, glm::vec2 hi, uint64_t id)
{
  return {0.5f * (lo + hi), 0.5f * (hi - lo), 0.0f, 0.0f, 2.0f, false, id};
}

Fleet::Settings deterministic()
{
  Fleet::Settings s;
  s.threads = 0; // (plans in update(): the same every run)
  return s;
}

bool touching(const Fleet& f, Fleet::AgentId a, Fleet::AgentId b)
{
  for (const Footprint& p : f.footprint(a))
    for (const Footprint& q : f.footprint(b))
      if (overlaps(p, q))
        return true;
  return false;
}

// Run until every agent has arrived or is blocked (or `seconds` pass);
// counts frames where any two agents touch.
int run(Fleet& f, float seconds, std::vector<Fleet::Event>* events = nullptr)
{
  int touches = 0;
  const std::vector<Fleet::AgentId> ids = f.ids();
  for (float t = 0.0f; t < seconds; t += 1.0f / 30.0f)
  {
    f.update(1.0 / 30.0);
    for (Fleet::Event& e : f.takeEvents())
      if (events)
        events->push_back(std::move(e));
    bool all = true;
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
      const Fleet::State st = f.state(ids[i]);
      all = all && (st == Fleet::State::Arrived || st == Fleet::State::Blocked);
      for (std::size_t j = i + 1; j < ids.size(); ++j)
        touches += touching(f, ids[i], ids[j]);
    }
    if (all)
      break;
  }
  return touches;
}

// A straight path a game made: along x from `x0` to `x1` at y, profiled.
std::shared_ptr<const AgentPath> straightPath(float x0, float x1, float y)
{
  vehicle::VehiclePath p;
  for (float x = x0; x <= x1 + 1.0e-4f; x += 0.1f)
  {
    vehicle::PathPoint q;
    q.s = x - x0;
    q.pose = {{x, y}, 0.0f};
    p.points.push_back(q);
  }
  p.start = p.points.front().pose;
  vehicle::profile(p, vehicle::modelOf(car()));
  return std::make_shared<vehicle::VehicleAgentPath>(std::move(p));
}

// A straight path a game made: `length` m from `from`, heading `heading`.
std::shared_ptr<const AgentPath>
linePath(glm::vec2 from, float heading, float length)
{
  vehicle::VehiclePath p;
  const glm::vec2 dir{std::cos(heading), std::sin(heading)};
  for (float s = 0.0f; s <= length + 1.0e-4f; s += 0.1f)
  {
    vehicle::PathPoint q;
    q.s = s;
    q.pose = {from + dir * s, heading};
    p.points.push_back(q);
  }
  p.start = p.points.front().pose;
  vehicle::profile(p, vehicle::modelOf(car()));
  return std::make_shared<vehicle::VehicleAgentPath>(std::move(p));
}
} // namespace

int main()
{
  vehicle::addKind();

  TEST("the vehicle kind should be registered, and unknown kinds refused")
  {
    CHECK(AgentKinds::find("vehicle") != nullptr);
    Fleet f(deterministic());
    AgentSpec walker = car();
    walker.kind = "walker";
    CHECK(!f.add(1, walker, {}));
    CHECK(f.add(2, car(), {}));
  }

  TEST("one agent should plan, drive and arrive at its goal")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{
                    wall({10.0f, -3.0f}, {11.0f, 3.0f}, 7)}),
                nullptr,
                nullptr});
    f.add(1, car(), {{0.0f, 0.0f}, 0.0f});
    f.requestGoal(1, {{{22.0f, 0.0f}, 0.0f}});
    std::vector<Fleet::Event> events;
    run(f, 60.0f, &events);
    CHECK(f.state(1) == Fleet::State::Arrived);
    CHECK(glm::length(f.pose(1).position - glm::vec2(22.0f, 0.0f)) < 0.05f);
    CHECK(!events.empty() && events.back().kind == Fleet::Event::Kind::Arrived);
    CHECK(f.counters(1).plans == 1);
  }

  TEST("an agent shut in a pen should be reported blocked, with blockers")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{
                    wall({20.0f, -6.0f}, {21.0f, 6.0f}, 1),
                    wall({33.0f, -6.0f}, {34.0f, 6.0f}, 2),
                    wall({20.0f, -7.0f}, {34.0f, -6.0f}, 3),
                    wall({20.0f, 6.0f}, {34.0f, 7.0f}, 4)}),
                nullptr,
                nullptr});
    f.add(1, car(), {{25.0f, 0.0f}, 0.0f});
    std::vector<Fleet::Event> events;
    f.requestGoal(1, {{{0.0f, 0.0f}, 0.0f}});
    run(f, 30.0f, &events);
    CHECK(f.state(1) == Fleet::State::Blocked);
    CHECK(!f.blockers(1).empty());
  }

  TEST("crossing a working machine's way: the other holds short, the "
       "working one never stops")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                nullptr,
                nullptr});
    f.add(1, car(), {{-20.0f, 0.0f}, 0.0f});
    f.add(2, car(), {{0.0f, -16.0f}, kPi / 2});
    f.setPriority(2, 5);
    f.requestGoal(1, {{{25.0f, 0.0f}, 0.0f}});
    f.requestGoal(2, {{{0.0f, 25.0f}, kPi / 2}});
    const int touches = run(f, 90.0f);
    CHECK(touches == 0);
    CHECK(f.state(1) == Fleet::State::Arrived);
    CHECK(f.state(2) == Fleet::State::Arrived);
    CHECK(f.counters(2).heldSeconds == 0.0f);
    CHECK(f.counters(2).waitSeconds == 0.0f);
    CHECK(f.counters(2).replans == 0);
    CHECK(f.traffic().deadlocks().empty());
  }

  TEST("head-on in a lane with a bay: one gives way, nobody touches, both "
       "arrive, only the one giving way re-plans")
  {
    // A lane 4.4 m wide along x, a bay 10 m long opening off it at y > 2.2.
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{
                    wall({-40.0f, -3.2f}, {40.0f, -2.2f}, 1),
                    wall({-40.0f, 2.2f}, {-5.0f, 3.2f}, 2),
                    wall({5.0f, 2.2f}, {40.0f, 3.2f}, 3),
                    wall({-5.0f, 9.0f}, {5.0f, 10.0f}, 4),
                    wall({-6.0f, 2.2f}, {-5.0f, 10.0f}, 5),
                    wall({5.0f, 2.2f}, {6.0f, 10.0f}, 6)}),
                nullptr,
                nullptr});
    f.add(1, car(), {{-30.0f, 0.0f}, 0.0f});
    f.add(2, car(), {{30.0f, 0.0f}, kPi});
    f.requestGoal(1, {{{30.0f, 0.0f}, 0.0f}});
    f.requestGoal(2, {{{-30.0f, 0.0f}, kPi}});
    const int touches = run(f, 200.0f);
    CHECK(touches == 0);
    CHECK(f.state(1) == Fleet::State::Arrived);
    CHECK(f.state(2) == Fleet::State::Arrived);
    CHECK(f.traffic().deadlocks().empty());
    CHECK(f.counters(1).replans == 0 || f.counters(2).replans == 0);
  }

  TEST("a game's path should be held to its speed cap: 0 stands it, none "
       "lets it on to the end")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                nullptr,
                nullptr});
    f.add(1, car(), {{0.0f, 0.0f}, 0.0f});
    f.setPath(1, straightPath(0.0f, 30.0f, 0.0f));
    f.setSpeedCap(1, 0.0f);
    for (int i = 0; i < 90; ++i)
      f.update(1.0 / 30.0);
    CHECK(f.along(1) < 0.2f);
    f.setSpeedCap(1, 2.0f);
    float fastest = 0.0f;
    for (int i = 0; i < 300; ++i)
    {
      f.update(1.0 / 30.0);
      fastest = std::max(fastest, std::abs(f.speed(1)));
    }
    CHECK(fastest <= 2.0f + 1.0e-3f);
    f.setSpeedCap(1, std::numeric_limits<float>::infinity());
    run(f, 60.0f);
    CHECK(f.state(1) == Fleet::State::Arrived);
    CHECK(f.counters(1).plans == 0);
  }

  TEST("a game's path held by one parked on it should be handed back "
       "(NeedsPlan), the fleet planning nothing itself")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                nullptr,
                nullptr});
    f.add(1, car(), {{0.0f, 0.0f}, 0.0f});
    f.add(2, car(), {{20.0f, 0.0f}, 0.0f}); // (parked on its way)
    f.setPriority(2, 5);
    f.setPath(1, straightPath(0.0f, 40.0f, 0.0f));
    std::vector<Fleet::Event> events;
    for (int i = 0; i < 30 * 30; ++i)
    {
      f.update(1.0 / 30.0);
      for (Fleet::Event& e : f.takeEvents())
        events.push_back(std::move(e));
    }
    bool handedBack = false;
    for (const Fleet::Event& e : events)
      handedBack = handedBack || (e.kind == Fleet::Event::Kind::NeedsPlan &&
                                  e.agent == 1 && e.other == 2);
    CHECK(handedBack);
    CHECK(f.counters(1).plans == 0);
    CHECK(!touching(f, 1, 2));
  }

  // Mutual hold: the one giving way stands on the other's way still holding
  // its path (its game holds it: speed cap 0, so it neither clears out nor
  // goes on), the one with the right of way waits for its body. Never both
  // hold: once it has stood parkedAfter, the one with the right of way plans
  // round it (or, driving a game's path, hands it back to plan).
  TEST("a pair waiting on each other (the one giving way held on the other's "
       "way, its path kept): the one with the right of way plans round it")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                nullptr,
                nullptr});
    f.add(1, car(), {{0.0f, -1.8f}, kPi / 2}); // (across 2's way)
    f.setPath(1, linePath({0.0f, -1.8f}, kPi / 2, 25.0f));
    f.setSpeedCap(1, 0.0f);
    f.add(2, car(), {{-30.0f, 0.0f}, 0.0f});
    f.setPriority(2, 5);
    f.requestGoal(2, {{{30.0f, 0.0f}, 0.0f}});
    int touches = 0;
    for (int i = 0; i < 30 * 90 && f.state(2) != Fleet::State::Arrived; ++i)
    {
      f.update(1.0 / 30.0);
      f.takeEvents();
      touches += touching(f, 1, 2);
    }
    CHECK(f.state(2) == Fleet::State::Arrived);
    CHECK(f.counters(2).roundParked >= 1);
    CHECK(touches == 0);
  }

  TEST("a pair waiting on each other, the one with the right of way on a "
       "game's path: it's handed back (NeedsPlan), never left waiting")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                nullptr,
                nullptr});
    f.add(1, car(), {{0.0f, -1.8f}, kPi / 2});
    f.setPath(1, linePath({0.0f, -1.8f}, kPi / 2, 25.0f));
    f.setSpeedCap(1, 0.0f);
    f.add(2, car(), {{-30.0f, 0.0f}, 0.0f});
    f.setPriority(2, 5);
    f.setPath(2, straightPath(-30.0f, 30.0f, 0.0f));
    bool handedBack = false;
    for (int i = 0; i < 30 * 60 && !handedBack; ++i)
    {
      f.update(1.0 / 30.0);
      for (const Fleet::Event& e : f.takeEvents())
        handedBack = handedBack || (e.kind == Fleet::Event::Kind::NeedsPlan &&
                                    e.agent == 2 && e.other == 1);
    }
    CHECK(handedBack);
    CHECK(!touching(f, 1, 2));
  }

  TEST("the speed profile should keep to a caller's limit per point")
  {
    vehicle::VehiclePath p;
    std::vector<float> limits;
    for (int i = 0; i <= 400; ++i)
    {
      vehicle::PathPoint q;
      q.s = 0.1f * static_cast<float>(i);
      q.pose = {{q.s, 0.0f}, 0.0f};
      p.points.push_back(q);
      limits.push_back(q.s > 15.0f && q.s < 25.0f ? 1.0f : 100.0f);
    }
    vehicle::profile(p,
                     vehicle::modelOf(car()),
                     std::numeric_limits<float>::infinity(),
                     0.0f,
                     &limits);
    bool kept = true, faster = false;
    for (std::size_t i = 0; i < p.points.size(); ++i)
    {
      kept = kept && p.points[i].speed <= limits[i] + 1.0e-4f;
      faster = faster || p.points[i].speed > 2.0f;
    }
    CHECK(kept);
    CHECK(faster);
  }

  return testing::report("fleetTests");
}
