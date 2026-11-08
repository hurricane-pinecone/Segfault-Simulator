#include "../../testHarness.h"

#include <engine/runtime/navigation/fleet.h>
#include <engine/runtime/navigation/vehicle/follower.h>
#include <engine/runtime/navigation/vehicle/speedProfile.h>
#include <engine/runtime/navigation/vehicle/vehicleKind.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
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

  return testing::report("fleetTests");
}
