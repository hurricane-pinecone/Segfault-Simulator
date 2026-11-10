#include "../../testHarness.h"

#include <engine/core/logger/logger.h>
#include <engine/runtime/navigation/fleet.h>
#include <engine/runtime/navigation/vehicle/follower.h>
#include <engine/runtime/navigation/vehicle/speedProfile.h>
#include <engine/runtime/navigation/vehicle/vehicleKind.h>
#include <engine/runtime/navigation/vehicle/vehicleModel.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <thread>

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

// (No scheduler: plans run in update(), the same every run.)
Fleet::Settings deterministic() { return Fleet::Settings{}; }

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

// The rest of a game's path from `s` metres along, as a fresh stretch (a
// game hands its route over a stretch at a time: each a new path).
std::shared_ptr<const AgentPath>
restOf(const AgentPath& path, const AgentSpec& spec, float s, float speed)
{
  const auto* v = dynamic_cast<const vehicle::VehicleAgentPath*>(&path);
  if (!v)
    return nullptr;
  vehicle::VehiclePath p;
  for (const vehicle::PathPoint& q : v->path.points)
    if (q.s >= s - 1.0e-4f)
    {
      vehicle::PathPoint r = q;
      r.s -= s;
      p.points.push_back(r);
    }
  if (p.points.size() < 2)
    return nullptr;
  p.start = p.points.front().pose;
  p.startHitch = p.points.front().hitch;
  vehicle::profile(p,
                   vehicle::modelOf(spec),
                   std::numeric_limits<float>::infinity(),
                   std::abs(speed));
  return std::make_shared<vehicle::VehicleAgentPath>(std::move(p));
}

// A game's path for `spec`: `first` m straight from `from` along `heading`
// (backing up, < 0), a turn of `angle` rad (+ left) at `radius`, then
// `then` m straight.
std::shared_ptr<const AgentPath> turnPath(const AgentSpec& spec,
                                          glm::vec2 from,
                                          float heading,
                                          float first,
                                          float radius,
                                          float angle,
                                          float then)
{
  vehicle::VehiclePath p;
  glm::vec2 at = from;
  float h = heading, s = 0.0f;
  const float step = 0.1f;
  const int gear = first < 0.0f ? -1 : 1;
  first = std::abs(first);
  const auto put = [&](float curvature)
  {
    vehicle::PathPoint q;
    q.s = s;
    q.pose = {at, h};
    q.curvature = curvature;
    q.gear = gear;
    p.points.push_back(q);
  };
  put(0.0f);
  const float turn = std::abs(angle) * radius;
  const float k = (angle < 0.0f ? -1.0f : 1.0f) / radius;
  for (float d = step; d <= first + turn + then + 1.0e-4f; d += step)
  {
    const bool turning = d > first && d <= first + turn;
    if (turning)
      h += k * step;
    at += float(gear) * step * glm::vec2(std::cos(h), std::sin(h));
    s += step;
    put(turning ? k : 0.0f);
  }
  p.start = p.points.front().pose;
  vehicle::profile(p, vehicle::modelOf(spec));
  return std::make_shared<vehicle::VehicleAgentPath>(std::move(p));
}

// The least gap between two agents' boxes (0: touching).
float gapBetween(const Fleet& f, Fleet::AgentId a, Fleet::AgentId b)
{
  float lo = 0.0f, hi = 50.0f;
  for (int i = 0; i < 24; ++i)
  {
    const float m = 0.5f * (lo + hi);
    bool near = false;
    for (const Footprint& p : f.footprint(a))
      for (const Footprint& q : f.footprint(b))
        near = near || overlaps(p, q, m);
    (near ? hi : lo) = m;
  }
  return lo;
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

  TEST("crossing a working machine's way: the other keeps out of its "
       "reservation, the working one never stops")
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
    CHECK(f.counters(2).replans == 0);
  }

  TEST("head-on in a lane with a bay: the lower one waits in the bay, "
       "nobody touches, both arrive, at most one wait spot each")
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
    // (Sent to a wait spot at most once, re-planning for traffic at most
    // three times (the wait spot, its way on, round the other where it
    // arrived).)
    for (const Fleet::AgentId id : {Fleet::AgentId{1}, Fleet::AgentId{2}})
    {
      CHECK(f.counters(id).waitSpots <= 1);
      CHECK(f.counters(id).replans <= 3);
    }
    CHECK(f.counters(1).waitSpots == 0); // (the higher one never)
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

  // Item 0 (A2z): one with nothing to do (parked, put away, idle) is a static
  // solid from the moment it stops, never traffic: no giving way to it, no
  // waiting on it.
  TEST("one standing with nothing to do on the only lane: the other plans "
       "round it at once, nobody gives way")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{
                    wall({-10.0f, 4.0f}, {50.0f, 5.0f}, 100),
                    wall({-10.0f, -5.0f}, {50.0f, -4.0f}, 101)}),
                nullptr,
                nullptr});
    f.add(2, car(), {{15.0f, -2.0f}, 0.0f}); // (parked, half the lane)
    f.setPriority(2, 5);                     // (priority never counts)
    CHECK(f.solid(2));
    f.add(1, car(), {{0.0f, 0.0f}, 0.0f});
    f.requestGoal(1, {{{35.0f, 0.0f}, 0.0f}});
    std::vector<Fleet::Event> events;
    const int touches = run(f, 60.0f, &events);
    int yielding = 0;
    for (const Fleet::Event& e : events)
      yielding += e.kind == Fleet::Event::Kind::Yielding;
    CHECK(f.state(1) == Fleet::State::Arrived);
    CHECK(yielding == 0);
    CHECK(f.counters(1).heldSeconds == 0.0f);
    CHECK(f.counters(1).replans == 0); // (round it in its first plan)
    CHECK(touches == 0);
    CHECK(f.solid(1)); // (arrived: nothing to do, a solid at once)
  }

  TEST("a game's path through one that has stopped with nothing to do: "
       "handed back at once (NeedsPlan), no giving way, stood short of it")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                nullptr,
                nullptr});
    f.add(1, car(), {{0.0f, 0.0f}, 0.0f});
    f.add(2, car(), {{20.0f, 0.0f}, 0.0f});
    f.setPath(1, straightPath(0.0f, 40.0f, 0.0f));
    CHECK(!f.solid(1)); // (a path to drive: traffic)
    std::vector<Fleet::Event> events;
    int frames = 0;
    bool handedBack = false;
    for (; frames < 30 * 30 && !handedBack; ++frames)
    {
      f.update(1.0 / 30.0);
      for (Fleet::Event& e : f.takeEvents())
      {
        handedBack = handedBack || (e.kind == Fleet::Event::Kind::NeedsPlan &&
                                    e.agent == 1 && e.other == 2);
        events.push_back(std::move(e));
      }
      CHECK(!touching(f, 1, 2));
    }
    int yielding = 0;
    for (const Fleet::Event& e : events)
      yielding += e.kind == Fleet::Event::Kind::Yielding;
    CHECK(handedBack);
    CHECK(yielding == 0);
    CHECK(frames < 30 * 10); // (braked to a stand, never waited on)
    CHECK(f.counters(1).heldSeconds == 0.0f);
    CHECK(!f.solid(1)); // (stood for its game's plan: not idle)
  }

  TEST("one its game is planning a way for is about to move: never a solid "
       "meanwhile, a solid again once it isn't")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                nullptr,
                nullptr});
    f.add(1, car(), {{0.0f, 0.0f}, 0.0f});
    f.add(2, car(), {{8.0f, 0.0f}, 0.0f});
    f.setSpeedCap(1, 0.0f); // (its game holds it while it plans)
    f.update(1.0 / 30.0);
    CHECK(f.solid(1));
    f.setPlanning(1, true);
    for (int frame = 0; frame < 30 * 5; ++frame)
      f.update(1.0 / 30.0); // (stood past parkedAfter)
    CHECK(!f.solid(1));
    f.setPlanning(1, false);
    f.update(1.0 / 30.0);
    CHECK(f.solid(1));
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
    CHECK(f.counters(2).roundSolid >= 1);
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

  // Two machines on their games' paths, both heading for the same ground,
  // one with a wide header on its front: each new stretch a game hands over
  // is a new claim, and either may be handed back (NeedsPlan) and planned
  // again from where it stands. However often the right of way changes
  // hands, they never close in on each other.
  TEST("two game-led machines giving way to each other (fresh stretches, "
       "handed back and planned again) never touch, a front header too")
  {
    for (int variant = 0; variant < 6; ++variant)
    {
      Fleet f(deterministic());
      f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                  nullptr,
                  nullptr});
      AgentSpec combine = car();
      combine.boxes.push_back({3.6f, 4.8f, 2.0f, 0.0f, 0.6f, 1.6f});
      f.add(1, combine, {{-25.0f, 0.0f}, 0.0f});
      f.add(2, car(), {{0.0f, -22.0f + 1.5f * variant}, kPi / 2});
      const std::map<Fleet::AgentId, AgentGoal> goals{
          {1, {{{25.0f, 0.0f}, 0.0f}}}, {2, {{{0.0f, 22.0f}, kPi / 2}}}};
      f.setPath(1, straightPath(-25.0f, 25.0f, 0.0f));
      f.setPath(2,
                linePath({0.0f, -22.0f + 1.5f * variant},
                         kPi / 2,
                         44.0f - 1.5f * variant));
      float least = 1.0e9f, every = 0.0f;
      bool arrived1 = false, arrived2 = false;
      for (int i = 0; i < 30 * 120 && !(arrived1 && arrived2); ++i)
      {
        f.update(1.0 / 30.0);
        for (const Fleet::Event& e : f.takeEvents())
        {
          if (e.kind == Fleet::Event::Kind::NeedsPlan)
          {
            PlanAsk ask;
            ask.from = f.pose(e.agent);
            ask.goals = {goals.at(e.agent)};
            const AgentPlan plan = f.planNow(e.agent, ask);
            if (plan.path)
              f.setPath(e.agent, plan.path);
          }
          if (e.kind == Fleet::Event::Kind::Arrived)
            (e.agent == 1 ? arrived1 : arrived2) = true;
        }
        every += 1.0f / 30.0f;
        if (every >= 1.0f)
        {
          // (A game handing its route over a stretch at a time.)
          every = 0.0f;
          for (Fleet::AgentId id : {Fleet::AgentId(1), Fleet::AgentId(2)})
            if (const AgentPath* p = f.path(id);
                p && f.state(id) == Fleet::State::Moving &&
                p->length() - f.along(id) > 2.0f)
              if (auto rest = restOf(*p, *f.spec(id), f.along(id), f.speed(id)))
                f.setPath(id, rest);
        }
        least = std::min(least, gapBetween(f, 1, 2));
      }
      CHECK(least >= 0.02f);
      CHECK(arrived1 && arrived2);
    }
  }

  // A combine with a wide header stands waiting for its route; a tractor's
  // game path runs along its way, just clear of the header's side (the
  // planner passes one standing at its own small gap). The combine then
  // sets off along its way with the tractor still beside it (the a2x yard
  // clash): it may pass the tractor once it stands, but never nearer than
  // the planners' own gap (2 cm), header included.
  TEST("one setting off beside another's body passes it no nearer than the "
       "planners' gap, a front header too")
  {
    for (int variant = 0; variant < 4; ++variant)
    {
      Fleet f(deterministic());
      f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                  nullptr,
                  nullptr});
      AgentSpec combine = car();
      combine.boxes = {{-6.38f, 2.34f, 1.76f, 0.1f, 0.0f, 4.0f},
                       {2.09f, 4.78f, 3.17f, 0.0f, 0.6f, 1.6f}};
      AgentSpec tractor = car();
      tractor.boxes = {{-1.0f, 3.96f, 1.31f, 0.0f, 0.0f, 3.5f}};
      f.add(1, combine, {{7.8f, 28.3f}, 0.0f});
      // (Its side 2..5 cm off the header's side.)
      const float y = 28.3f - 3.17f - 1.31f - 0.02f - 0.01f * variant;
      f.add(2, tractor, {{-5.0f, y}, 0.0f});
      f.setPath(2, linePath({-5.0f, y}, 0.0f, 19.0f));
      float least = 1.0e9f;
      bool set = false, arrived1 = false;
      for (int i = 0; i < 30 * 90 && !arrived1; ++i)
      {
        f.update(1.0 / 30.0);
        for (const Fleet::Event& e : f.takeEvents())
          if (e.kind == Fleet::Event::Kind::Arrived && e.agent == 1)
            arrived1 = true;
        if (!set && f.pose(2).position.x > 2.0f)
        {
          // (Its route comes as the tractor's front draws level.)
          set = true;
          f.setPath(1, straightPath(7.8f, 40.0f, 28.3f));
        }
        if (set && std::abs(f.speed(1)) > 0.01f)
          least = std::min(least, gapBetween(f, 1, 2));
      }
      CHECK(least >= 0.02f);
    }
  }

  // The a3a yard: a combine (a header on its front) stands waiting for its
  // route while a tractor's game path crosses in front of it; the route
  // comes with the tractor's front in its way. The combine ranks higher but
  // stands for the tractor's body, so it reserves only up to there: the
  // tractor (already in front of it) carries on through, never stands in
  // front of the header, is never a solid there; the combine is never
  // handed back to plan round it and never stands for a reservation.
  TEST("a tractor crossing in front of a header carries on through: the "
       "combine stands for its body only, nobody plans round anybody")
  {
    Fleet::Settings settings = deterministic();
    settings.parkedAfter = 0.05f;
    Fleet f(settings);
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                nullptr,
                nullptr});
    AgentSpec combine = car();
    combine.boxes = {{-6.38f, 2.34f, 1.76f, 0.1f, 0.0f, 4.0f},
                     {1.92f, 4.96f, 3.17f, 0.0f, 0.25f, 2.97f}};
    AgentSpec tractor = car();
    tractor.boxes = {{-1.0f, 3.96f, 1.31f, 0.0f, 0.0f, 3.5f}};
    f.add(1, combine, {{7.8f, 28.3f}, 0.0f});
    f.setPriority(1, 1); // (the combine going about first)
    const glm::vec2 from{10.0f, 15.0f};
    const float heading = 1.05f;
    f.add(2, tractor, {from, heading});
    f.setPath(2, linePath(from, heading, 26.0f));
    const std::map<Fleet::AgentId, AgentGoal> goals{
        {1, {{{40.0f, 28.3f}, 0.0f}}},
        {2, {{from + 26.0f * unit(heading), heading}}}};
    bool set = false, arrived1 = false, arrived2 = false;
    bool solidWhileGiving = false, combineHanded = false;
    int combineYields = 0, tractorYields = 0;
    float least = 1.0e9f;
    for (int i = 0; i < 30 * 120 && !(arrived1 && arrived2); ++i)
    {
      f.update(1.0 / 30.0);
      for (const Fleet::Event& e : f.takeEvents())
      {
        if (e.kind == Fleet::Event::Kind::NeedsPlan)
        {
          combineHanded = combineHanded || (e.agent == 1 && e.other == 2);
          PlanAsk ask;
          ask.from = f.pose(e.agent);
          ask.goals = {goals.at(e.agent)};
          const AgentPlan plan = f.planNow(e.agent, ask);
          if (plan.path)
            f.setPath(e.agent, plan.path);
        }
        if (e.kind == Fleet::Event::Kind::Yielding)
          ++(e.agent == 1 ? combineYields : tractorYields);
        if (e.kind == Fleet::Event::Kind::Arrived)
          (e.agent == 1 ? arrived1 : arrived2) = true;
      }
      if (!set && f.pose(2).position.y > 22.0f)
      {
        // (Its route comes with the tractor's front in its way.)
        set = true;
        f.setPath(1, straightPath(7.8f, 40.0f, 28.3f));
      }
      solidWhileGiving = solidWhileGiving ||
                         (f.state(2) == Fleet::State::Yielding && f.solid(2));
      least = std::min(least, gapBetween(f, 1, 2));
    }
    CHECK(set);
    CHECK(!solidWhileGiving);
    CHECK(!combineHanded);
    CHECK(f.counters(1).roundSolid == 0);
    CHECK(f.counters(1).waitSpots == 0);
    CHECK(f.counters(2).waitSpots == 0);
    CHECK(combineYields == 0);
    CHECK(least >= 0.02f);
    CHECK(arrived1 && arrived2);
  }

  // Head-on through a gateway (a 5 m gap in a long wall): the gap is a
  // narrow place (zone), held by one at a time; found from the statics,
  // nothing placed by hand.
  TEST("head-on in a gateway: one at a time through it, nobody touches, "
       "both arrive, no flip-flop")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{
                    wall({-1.0f, -60.0f}, {1.0f, -2.5f}, 1),
                    wall({-1.0f, 2.5f}, {1.0f, 60.0f}, 2)}),
                nullptr,
                nullptr});
    f.add(1, car(), {{-25.0f, -8.0f}, 0.0f});
    f.add(2, car(), {{25.0f, 8.0f}, kPi});
    f.requestGoal(1, {{{25.0f, -8.0f}, 0.0f}});
    f.requestGoal(2, {{{-25.0f, 8.0f}, kPi}});
    std::vector<Fleet::Event> events;
    bool bothIn = false;
    int touches = 0;
    for (int i = 0; i < 30 * 200; ++i)
    {
      f.update(1.0 / 30.0);
      for (Fleet::Event& e : f.takeEvents())
        events.push_back(std::move(e));
      touches += touching(f, 1, 2);
      bothIn = bothIn || (std::abs(f.pose(1).position.x) < 3.0f &&
                          std::abs(f.pose(2).position.x) < 3.0f);
      if (f.state(1) == Fleet::State::Arrived &&
          f.state(2) == Fleet::State::Arrived)
        break;
    }
    int yields = 0;
    for (const Fleet::Event& e : events)
      yields += e.kind == Fleet::Event::Kind::Yielding;
    CHECK(touches == 0);
    CHECK(!bothIn);
    CHECK(f.state(1) == Fleet::State::Arrived);
    CHECK(f.state(2) == Fleet::State::Arrived);
    CHECK(yields <= 2); // (stood for the other once or twice, no flip-flop)
  }

  // A cart coming to meet a working combine (its game path along a pass):
  // it keeps out of the combine's reservation, crossing behind it, and
  // arrives beside the way the combine has passed.
  TEST("a cart meeting a working combine keeps out of its reservation: "
       "the combine never stands, nobody touches")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                nullptr,
                nullptr});
    AgentSpec combine = car();
    combine.boxes.push_back({3.6f, 4.8f, 3.0f, 0.0f, 0.6f, 1.6f});
    f.add(1, combine, {{-20.0f, 0.0f}, 0.0f});
    f.setPriority(1, 4);
    f.setPath(1, straightPath(-20.0f, 30.0f, 0.0f));
    f.add(2, car(), {{0.0f, -25.0f}, kPi / 2});
    f.setPriority(2, 3);
    f.requestGoal(2, {{{0.0f, 12.0f}, kPi / 2}});
    const int touches = run(f, 120.0f);
    CHECK(touches == 0);
    CHECK(f.state(1) == Fleet::State::Arrived);
    CHECK(f.state(2) == Fleet::State::Arrived);
    CHECK(f.counters(1).heldSeconds == 0.0f);
    CHECK(f.counters(1).hardStops == 0);
  }

  // A semi parks half across a lane after the combine coming along it was
  // planned: a static obstacle, planned round at once, never waited on.
  TEST("a semi parked on the lane after the other was planned: planned "
       "round, never waited on, nobody touches")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{
                    wall({-30.0f, 6.0f}, {60.0f, 7.0f}, 100),
                    wall({-30.0f, -7.0f}, {60.0f, -6.0f}, 101)}),
                nullptr,
                nullptr});
    AgentSpec semi = car();
    semi.boxes = {{-1.5f, 14.5f, 1.25f, 0.0f, 0.0f, 4.0f}};
    semi.minRadius = 12.0f;
    semi.wheelbase = 6.0f;
    f.add(2, semi, {{5.0f, -3.5f}, 0.0f});
    f.add(1, car(), {{-25.0f, 0.0f}, 0.0f});
    f.setPriority(1, 1);
    f.requestGoal(1, {{{50.0f, 0.0f}, 0.0f}});
    f.update(1.0 / 30.0); // (planned with the semi elsewhere...)
    f.requestGoal(2, {{{15.0f, -2.5f}, 0.0f}}); // (...then it parks here)
    const int touches = run(f, 120.0f);
    CHECK(touches == 0);
    CHECK(f.state(1) == Fleet::State::Arrived);
    CHECK(f.state(2) == Fleet::State::Arrived);
    CHECK(f.solid(2));
    CHECK(f.counters(1).waitSpots == 0);
  }

  // Deadlocks (D3b). A one-lane yard lane, closed at its east end, a bay off
  // its north side near its west end and a shed bay off its south side at
  // its east end (where the higher one is going). Two coming head-on along
  // it can't pass: the lower one has nowhere clear of the higher one's way
  // (all the lane on east of it, and the shed bay), so each stands for the
  // other: a wait-for cycle. The next lowest that can (the higher one)
  // backs into the north bay clear of the other's way, lets it through, and
  // goes on: the farm hands in a yard, one backs off to somewhere actually
  // clear.
  const auto yardLane = []
  {
    return std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{
        wall({-61.0f, -3.2f}, {30.0f, -2.2f}, 1), // south side, west of shed
        wall({38.0f, -3.2f}, {40.0f, -2.2f}, 9),  // south side, east of shed
        wall({-61.0f, 2.2f}, {-32.0f, 3.2f}, 2),  // north side, west of bay
        wall({-22.0f, 2.2f}, {40.0f, 3.2f}, 3),   // north side, east of bay
        wall({39.0f, -3.2f}, {40.0f, 3.2f}, 5),   // east end (the west's open)
        wall({-33.0f, 2.2f}, {-32.0f, 11.0f}, 6), // the north bay
        wall({-22.0f, 2.2f}, {-21.0f, 11.0f}, 7),
        wall({-33.0f, 10.0f}, {-21.0f, 11.0f}, 8),
        wall({29.0f, -12.0f}, {30.0f, -2.2f}, 10), // the shed bay: one wide
        wall({38.0f, -12.0f}, {39.0f, -2.2f}, 11),
        wall({29.0f, -12.0f}, {39.0f, -11.0f}, 12)});
  };
  // (The higher one's goal: in the shed bay, facing in.)
  const AgentGoal shed{{{34.0f, -5.0f}, -kPi / 2}};
  // Runs till all arrive (or `seconds`): touches, and whether all arrived.
  const auto runAll = [](Fleet& f, float seconds, int& touches)
  {
    touches = 0;
    const std::vector<Fleet::AgentId> ids = f.ids();
    for (float t = 0.0f; t < seconds; t += 1.0f / 30.0f)
    {
      f.update(1.0 / 30.0);
      f.takeEvents();
      bool all = true;
      for (std::size_t i = 0; i < ids.size(); ++i)
      {
        all = all && f.state(ids[i]) == Fleet::State::Arrived;
        for (std::size_t j = i + 1; j < ids.size(); ++j)
          touches += touching(f, ids[i], ids[j]);
      }
      if (all)
        return true;
    }
    return false;
  };

  TEST("a 2-agent yard deadlock (head-on in a closed lane, the lower one "
       "with nowhere to go): the higher backs into the bay, both arrive")
  {
    Fleet f(deterministic());
    f.setWorld({yardLane(), nullptr, nullptr});
    f.add(1, car(), {{-10.0f, 0.0f}, 0.0f});
    f.setPriority(1, 5);
    f.add(2, car(), {{10.0f, 0.0f}, kPi});
    f.requestGoal(1, shed);
    f.requestGoal(2, {{{-72.0f, 0.0f}, kPi}});
    int touches = 0;
    const bool arrived = runAll(f, 240.0f, touches);
    CHECK(arrived);
    CHECK(touches == 0);
    const Fleet::DeadlockStats d = f.deadlockStats();
    CHECK(d.found >= 1);
    CHECK(d.broken == d.found);
    CHECK(d.longest <= 10.0f);
    // (The lower one tried first and couldn't; the higher one gave way once.)
    CHECK(f.counters(2).waitSpots >= 1);
    CHECK(f.counters(2).deadlockYields == 0);
    CHECK(f.counters(1).deadlockYields == 1);
  }

  TEST("a 3-agent yard deadlock (two in a queue meeting one head-on): the "
       "lowest in the cycle can't, the next yields clear of the whole "
       "queue, all arrive, nobody touches")
  {
    Fleet f(deterministic());
    f.setWorld({yardLane(), nullptr, nullptr});
    f.add(1, car(), {{-10.0f, 0.0f}, 0.0f});
    f.setPriority(1, 5);
    f.add(2, car(), {{8.0f, 0.0f}, kPi});
    f.setPriority(2, 3);
    f.add(3, car(), {{18.0f, 0.0f}, kPi});
    f.requestGoal(1, shed);
    f.requestGoal(2, {{{-72.0f, 5.0f}, kPi}});
    f.requestGoal(3, {{{-72.0f, -5.0f}, kPi}});
    int touches = 0;
    const bool arrived = runAll(f, 300.0f, touches);
    CHECK(arrived);
    CHECK(touches == 0);
    const Fleet::DeadlockStats d = f.deadlockStats();
    CHECK(d.found >= 1);
    CHECK(d.broken == d.found);
    CHECK(d.longest <= 10.0f);
    CHECK(f.counters(1).deadlockYields == 1);
    CHECK(f.counters(2).deadlockYields == 0);
    CHECK(f.counters(3).deadlockYields == 0);
  }

  TEST("a 2-agent yard deadlock where the lowest can get out of the way: "
       "it yields, the higher one never does")
  {
    // (The bay is east of both: the lower one, coming from the east, can
    // back into it; the higher one, west of the lower one, can't reach it.)
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{
                    wall({-46.0f, -3.2f}, {61.0f, -2.2f}, 1),
                    wall({-46.0f, 2.2f}, {22.0f, 3.2f}, 2),
                    wall({32.0f, 2.2f}, {61.0f, 3.2f}, 3),
                    wall({-46.0f, -3.2f}, {-45.0f, 3.2f}, 4),
                    wall({60.0f, -3.2f}, {61.0f, 3.2f}, 5),
                    wall({21.0f, 2.2f}, {22.0f, 11.0f}, 6),
                    wall({32.0f, 2.2f}, {33.0f, 11.0f}, 7),
                    wall({21.0f, 10.0f}, {33.0f, 11.0f}, 8)}),
                nullptr,
                nullptr});
    f.add(1, car(), {{-10.0f, 0.0f}, 0.0f});
    f.setPriority(1, 5);
    f.add(2, car(), {{10.0f, 0.0f}, kPi});
    f.requestGoal(1, {{{50.0f, 0.0f}, 0.0f}});
    f.requestGoal(2, {{{-35.0f, 0.0f}, kPi}});
    int touches = 0;
    const bool arrived = runAll(f, 240.0f, touches);
    CHECK(arrived);
    CHECK(touches == 0);
    CHECK(f.counters(1).deadlockYields == 0);
    CHECK(f.counters(1).waitSpots == 0);
    CHECK(f.counters(2).waitSpots >= 1);
    CHECK(f.deadlockStats().longest <= 10.0f);
  }

  TEST("crossing a higher one's way it will have cleared in time: the lower "
       "one eases off and drives through behind it, never standing")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                nullptr,
                nullptr});
    f.add(1, car(), {{-30.0f, 0.0f}, 0.0f});
    f.setPriority(1, 5);
    f.setPath(1, straightPath(-30.0f, 40.0f, 0.0f));
    f.add(2, car(), {{0.0f, -30.0f}, kPi / 2});
    f.setPath(2, linePath({0.0f, -30.0f}, kPi / 2, 60.0f));
    float slowest = 1.0e9f, fastest = 0.0f;
    bool eased = false;
    int touches = 0;
    for (int i = 0; i < 30 * 60; ++i)
    {
      f.update(1.0 / 30.0);
      f.takeEvents();
      touches += touching(f, 1, 2);
      const float v = std::abs(f.speed(2));
      eased = eased || std::isfinite(f.traffic().scheduleOf(2).easeTo);
      if (fastest > 1.0f && f.pose(2).position.y < 5.0f)
        slowest = std::min(slowest, v);
      fastest = std::max(fastest, v);
      if (f.state(1) == Fleet::State::Arrived &&
          f.state(2) == Fleet::State::Arrived)
        break;
    }
    CHECK(eased);
    CHECK(touches == 0);
    CHECK(slowest > 0.5f);
    // (Only its first frame, at rest before it set off.)
    CHECK(f.counters(2).heldSeconds < 0.05f);
    CHECK(f.state(2) == Fleet::State::Arrived);
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

  // --- Driven by hand (a player at the wheel) ---

  TEST("by hand, the throttle should pull away at the derived accel to the "
       "gear's top speed, and the wheel turn at its steer rate to full lock")
  {
    vehicle::HandDrive d;
    const vehicle::VehicleModel m = vehicle::modelOf(car());
    d.setModel(m);
    d.start({{0.0f, 0.0f}, 0.0f}, 0.0f, 0.0f);
    Controls c;
    c.throttle = 1.0f;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 60; ++i)
      d.step(c, dt);
    // (A second at its accel, straight ahead.)
    CHECK(std::abs(d.speed() - m.accel) < 0.05f);
    CHECK(d.pose().position.x > 0.4f && std::abs(d.pose().position.y) < 1e-3f);
    for (int i = 0; i < 60 * 20; ++i)
      d.step(c, dt);
    CHECK(std::abs(d.speed() - m.topSpeed) < 1.0e-3f);
    // Full lock asked: the wheel gets there at steerRate, no faster.
    c.steer = 1.0f;
    d.step(c, 0.1f);
    CHECK(std::abs(d.steerAngle() - m.steerRate * 0.1f) < 1.0e-4f);
    for (int i = 0; i < 60 * 5; ++i)
      d.step(c, dt);
    CHECK(std::abs(d.steerAngle() - d.fullLock()) < 1.0e-4f);
    CHECK(std::abs(1.0f / d.curvature() - m.minRadius) < 0.01f);
    // On full lock, no faster than the speed profile's own limit there.
    CHECK(d.speed() <= m.speedAt(1, d.curvature(), 0.0f) + 1.0e-3f);
    CHECK(d.speed() < m.topSpeed);
  }

  TEST("by hand, letting off should slow it at its accel, the brake add its "
       "brake rate, and a cap hold it")
  {
    vehicle::HandDrive d;
    const vehicle::VehicleModel m = vehicle::modelOf(car());
    d.setModel(m);
    d.start({{0.0f, 0.0f}, 0.0f}, 0.0f, 4.0f);
    Controls c;
    d.step(c, 0.5f);
    CHECK(std::abs(d.speed() - (4.0f - 0.5f * m.accel)) < 1.0e-3f);
    c.brake = 1.0f;
    d.step(c, 0.5f);
    CHECK(std::abs(d.speed() -
                   std::max(0.0f, 4.0f - m.accel - 0.5f * m.brake)) < 1e-3f);
    c = {};
    c.throttle = 1.0f;
    for (int i = 0; i < 600; ++i)
      d.step(c, 1.0f / 60.0f, 2.5f);
    CHECK(std::abs(d.speed() - 2.5f) < 1.0e-3f);
  }

  TEST("by hand, the other gear should brake it to a stand, stand its "
       "shuttle, then drive it the other way")
  {
    vehicle::HandDrive d;
    vehicle::VehicleModel m = vehicle::modelOf(car());
    m.shuttleSeconds = 0.5f;
    d.setModel(m);
    d.start({{0.0f, 0.0f}, 0.0f}, 0.0f, 3.0f);
    Controls c;
    c.gear = -1;
    c.throttle = 1.0f;
    float t = 0.0f, stoodAt = -1.0f;
    float lowest = 1.0f;
    const float dt = 1.0f / 60.0f;
    for (; t < 10.0f; t += dt)
    {
      d.step(c, dt);
      lowest = std::min(lowest, d.speed());
      if (stoodAt < 0.0f && std::abs(d.speed()) < 1.0e-6f)
        stoodAt = t;
      if (d.speed() < -0.05f)
        break;
    }
    CHECK(stoodAt > 0.0f);
    // (It stood the shuttle before backing off.)
    CHECK(t - stoodAt >= 0.5f - 2.0f * dt);
    CHECK(d.speed() < 0.0f);
    for (int i = 0; i < 600; ++i)
      d.step(c, dt);
    CHECK(std::abs(d.speed() + m.reverseSpeed) < 1.0e-3f);
    CHECK(d.pose().position.x < 3.0f); // (came back past where it stood)
  }

  TEST("by hand, a trailer should swing on its hitch as its paths' trailers "
       "do, within its limit")
  {
    vehicle::HandDrive d;
    vehicle::VehicleModel m = vehicle::modelOf(car());
    m.trailer = vehicle::Trailer{};
    d.setModel(m);
    d.start({{0.0f, 0.0f}, 0.0f}, 0.0f, 0.0f);
    Controls c;
    c.throttle = 0.3f;
    c.steer = 1.0f;
    for (int i = 0; i < 60 * 20; ++i)
      d.step(c, 1.0f / 60.0f);
    CHECK(d.hitch() > 0.2f);
    CHECK(d.hitch() <= m.trailer->forwardLimit + 1.0e-5f);
  }

  TEST("a fleet agent driven by hand should go by its controls, be solid to "
       "the others, and stand where it is handed back")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                nullptr,
                nullptr});
    f.add(1, car(), {{0.0f, 0.0f}, 0.0f});
    f.requestGoal(1, {{{30.0f, 0.0f}, 0.0f}});
    CHECK(f.takeWheel(1));
    CHECK(f.handDriven(1));
    CHECK(f.goals(1).empty());
    Controls c;
    c.throttle = 1.0f;
    f.setControls(1, c);
    for (int i = 0; i < 120; ++i)
      f.update(1.0 / 60.0);
    CHECK(f.pose(1).position.x > 1.0f);
    CHECK(f.speed(1) > 1.0f);
    CHECK(f.path(1) == nullptr);
    CHECK(f.solid(1));
    CHECK(f.state(1) == Fleet::State::Moving);
    // (Its game's cap holds it, as on a path.)
    f.setSpeedCap(1, 0.0f);
    for (int i = 0; i < 600; ++i)
      f.update(1.0 / 60.0);
    CHECK(std::abs(f.speed(1)) < kStill);
    f.setSpeedCap(1, std::numeric_limits<float>::infinity());
    const Pose2 at = f.pose(1);
    f.handBack(1);
    CHECK(!f.handDriven(1));
    for (int i = 0; i < 60; ++i)
      f.update(1.0 / 60.0);
    CHECK(glm::length(f.pose(1).position - at.position) < 1.0e-4f);
    // Sent somewhere again, it plans and goes as any agent.
    f.requestGoal(1, {{{at.position.x + 10.0f, 0.0f}, 0.0f}});
    run(f, 60.0f);
    CHECK(f.state(1) == Fleet::State::Arrived);
  }

  TEST("on the planning scheduler, an agent should never drive the clear way "
       "so far: it stands till its finished plan is in, then drives that")
  {
    auto scheduler = std::make_shared<PlanScheduler>();
    Fleet::Settings settings;
    settings.nodeBudget = 300; // (searches on past several partial rounds)
    Fleet f(settings, scheduler);
    const World world{std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{
                          wall({10.0f, -8.0f}, {11.0f, 8.0f}, 7)}),
                      nullptr,
                      nullptr};
    f.setWorld(world);
    f.add(1, car(), {{0.0f, 0.0f}, 0.0f});
    PlanAsk ask;
    ask.world = world;
    ask.from = {{0.0f, 0.0f}, 0.0f};
    ask.goals = {{{{22.0f, 0.0f}, 0.0f}}};
    ask.nodeBudget = settings.nodeBudget;
    ask.budgetCap = settings.budgetCap;
    int partials = 0;
    const AgentPlan whole = AgentKinds::find("vehicle")->plan(
        car(), ask, [&](const AgentPlan&) { ++partials; });
    CHECK(whole.state == PlanState::Found);
    CHECK(partials > 0); // (the search did hand over clear ways so far)
    f.requestGoal(1, {{{22.0f, 0.0f}, 0.0f}});
    bool movedEarly = false, planned = false;
    for (int i = 0; i < 4000 && !f.path(1); ++i)
    {
      f.update(1.0 / 60.0);
      planned = planned || f.state(1) == Fleet::State::Planning;
      if (!f.path(1)) // (no finished plan yet: it hasn't moved)
        movedEarly = movedEarly || std::abs(f.speed(1)) > 0.0f ||
                     glm::length(f.pose(1).position) > 1e-4f;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(planned);
    CHECK(!movedEarly);
    CHECK(f.path(1) != nullptr);
    if (f.path(1) && whole.path)
      CHECK(std::abs(f.path(1)->length() - whole.path->length()) < 1e-4f);
    run(f, 60.0f);
    CHECK(f.state(1) == Fleet::State::Arrived);
  }

  TEST("look-ahead: the next way should be planned from where its path ends, "
       "needed by when it gets there")
  {
    auto scheduler = std::make_shared<PlanScheduler>();
    Fleet f(Fleet::Settings{}, scheduler);
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                nullptr,
                nullptr});
    f.add(1, car(), {{0.0f, 0.0f}, 0.0f});
    f.setPath(1, straightPath(0.0f, 30.0f, 0.0f));
    f.update(1.0 / 60.0);
    PlanAsk ask;
    ask.goals = {{{{30.0f, 20.0f}, 1.5708f}}};
    CHECK(f.planAhead(1, ask));
    CHECK(f.planningAhead(1));
    std::optional<AgentPlan> next;
    for (int i = 0; i < 4000 && !next; ++i)
    {
      next = f.ahead(1);
      if (!next)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(next.has_value());
    CHECK(!f.planningAhead(1));
    if (next && next->path && !next->path->samples.empty())
    {
      CHECK(next->state == PlanState::Found);
      CHECK(glm::length(next->path->samples.front().pose.position -
                        glm::vec2(30.0f, 0.0f)) < 1e-3f);
    }
    // (It drives the first to its end, then the one planned ahead.)
    run(f, 30.0f);
    CHECK(f.state(1) == Fleet::State::Arrived);
    if (next && next->path)
      f.setPath(1, next->path);
    run(f, 60.0f);
    CHECK(glm::length(f.pose(1).position - glm::vec2(30.0f, 20.0f)) < 0.1f);
  }

  TEST("a game's way handed back for a static obstacle, back the same from "
       "its game: not handed back again (no identical re-plan), stood short "
       "of it; a different way is")
  {
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                nullptr,
                nullptr});
    f.add(1, car(), {{0.0f, 0.0f}, 0.0f});
    f.add(2, car(), {{20.0f, 0.0f}, 0.0f});
    const auto handBacks = [&](int frames)
    {
      int n = 0;
      for (int frame = 0; frame < frames; ++frame)
      {
        f.update(1.0 / 30.0);
        for (const Fleet::Event& e : f.takeEvents())
          n += e.kind == Fleet::Event::Kind::NeedsPlan && e.agent == 1 &&
               e.other == 2;
        CHECK(!touching(f, 1, 2));
      }
      return n;
    };
    f.setPath(1, straightPath(0.0f, 40.0f, 0.0f));
    CHECK(handBacks(30 * 10) == 1);
    // (Its game planned it again and got the same way: once only.)
    f.setPath(1, straightPath(0.0f, 40.0f, 0.0f));
    CHECK(handBacks(30 * 10) == 0);
    CHECK(f.pose(1).position.x < 20.0f - 4.6f);
    // (A different way into it: handed back again.)
    f.setPath(1, straightPath(f.pose(1).position.x, 30.0f, 0.0f));
    CHECK(handBacks(30 * 10) == 1);
  }

  TEST("a deadlock nobody can get out of: the lowest stands, a static "
       "obstacle; the same ones forming it again aren't news (logged once)")
  {
    // (Nose to nose 0.1 m apart, each a game's way on into the other, their
    // games holding them (speed cap 0): neither can go to a wait spot.)
    Fleet f(deterministic());
    f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{}),
                nullptr,
                nullptr});
    f.add(1, car(), {{0.0f, 0.0f}, 0.0f});
    f.add(2, car(), {{7.3f, 0.0f}, kPi});
    f.setSpeedCap(1, 0.0f);
    f.setSpeedCap(2, 0.0f);
    int warned = 0;
    const sfs::Logger::SinkId sink = sfs::Logger::addSink(
        [&](sfs::Logger::Level level, const std::string& message)
        {
          if (level == sfs::Logger::Level::WARN &&
              message.find("nobody in it can get out") != std::string::npos)
            ++warned;
        });
    for (int round = 0; round < 3; ++round)
    {
      f.setPath(1, straightPath(0.0f, 30.0f, 0.0f));
      f.setPath(2, linePath({7.3f, 0.0f}, kPi, 30.0f));
      for (int frame = 0; frame < 30 * 8; ++frame)
      {
        f.update(1.0 / 30.0);
        f.takeEvents();
      }
      CHECK(!touching(f, 1, 2));
    }
    sfs::Logger::removeSink(sink);
    // (Its members planned again each round (stuck no more), the same ones
    // stand nobody able to give way again: no news.)
    CHECK(f.deadlockStats().found >= 1u);
    CHECK(warned == 1);
  }

  TEST("a cart standing in a working machine's way, head-on: it goes to a "
       "wait spot across the machine's turn; held in its way on the way "
       "there it would only be in its way, so it goes on out and the "
       "machine stands short of its way out: nobody touches or comes closer "
       "than the gap, nobody's stood at once, the machine never gives way, "
       "no deadlock")
  {
    // (The yard near-miss: a combine (a wide header ahead of its body)
    // coming east and turning right, south; a cart in its way facing it.
    // Its way to a wait spot crossed the combine's turn; held by the
    // combine's reservation half way out, it was stood at once in its way
    // and the combine's header came within cm of it.)
    AgentSpec combine = car();
    combine.boxes = {{-3.0f, 2.35f, 1.75f, 0.0f, 0.0f, 4.0f},
                     {1.9f, 4.4f, 3.17f, 0.0f, 0.6f, 2.8f}};
    combine.minRadius = 6.0f;
    combine.wheelbase = 3.5f;
    combine.topSpeed = 4.0f;
    AgentSpec cart = car();
    cart.boxes = {{-1.1f, 3.86f, 1.31f, 0.0f, 0.0f, 3.56f}};
    cart.topSpeed = 2.5f;
    for (const float from : {1.0f, 3.0f})
    {
      Fleet f(deterministic());
      // (Walled in to the south but for the combine's way south.)
      f.setWorld({std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{
                      wall({-40.0f, -40.0f}, {2.0f, -5.0f}, 1),
                      wall({12.0f, -40.0f}, {40.0f, -5.0f}, 2)}),
                  nullptr,
                  nullptr});
      f.add(1, combine, {{-from, 0.0f}, 0.0f});
      f.setPriority(1, 5);
      f.add(2, cart, {{10.0f, -1.0f}, kPi});
      f.setPath(
          1,
          turnPath(combine, {-from, 0.0f}, 0.0f, from, 7.0f, -kPi / 2, 30.0f));
      f.setPath(
          2, turnPath(cart, {10.0f, -1.0f}, kPi, 20.0f, 6.0f, 0.0f, 0.0f));
      float least = std::numeric_limits<float>::infinity();
      int touches = 0;
      for (float t = 0.0f; t < 60.0f; t += 1.0f / 30.0f)
      {
        f.update(1.0 / 30.0);
        f.takeEvents();
        least = std::min(least, gapBetween(f, 1, 2));
        touches += touching(f, 1, 2);
        if (f.state(1) == Fleet::State::Arrived)
          break;
      }
      std::printf("yard near-miss (combine %.0f m off): least gap %.3f m, "
                  "cart wait spots %d, hard stops %d/%d, combine held "
                  "%.1f s\n",
                  from,
                  least,
                  f.counters(2).waitSpots,
                  f.counters(1).hardStops,
                  f.counters(2).hardStops,
                  f.counters(1).heldSeconds);
      CHECK(f.state(1) == Fleet::State::Arrived);
      CHECK(touches == 0);
      CHECK(least >= vehicle::Tunables{}.gap);
      CHECK(f.counters(1).waitSpots == 0);
      CHECK(f.counters(1).deadlockYields == 0);
      CHECK(f.counters(2).waitSpots >= 1);
      CHECK(f.counters(1).hardStops == 0);
      CHECK(f.counters(2).hardStops == 0);
      CHECK(f.deadlockStats().found == 0);
    }
  }

  return testing::report("fleetTests");
}
