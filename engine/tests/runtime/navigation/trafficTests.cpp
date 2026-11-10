#include "../../testHarness.h"

#include <engine/runtime/navigation/traffic.h>

using namespace sfs::nav;

namespace
{
using Why = Traffic::Schedule::Why;

// An agent driving along +x or -x from x0 at `speed`: its claim for `ahead`
// seconds, a 5 x 2.6 m footprint a metre apart.
Traffic::Claim along(float x0,
                     float y,
                     float dir,
                     float speed,
                     int priority = 0,
                     float ahead = 8.0f)
{
  Traffic::Claim c;
  c.priority = priority;
  c.body = {{{x0, y}, {2.5f, 1.3f}, dir > 0 ? 0.0f : kPi}};
  for (float s = 1.0f; s <= speed * ahead; s += 1.0f)
  {
    Traffic::Sample t;
    t.s = s;
    t.enter = s / speed - 0.3f;
    t.exit = s / speed + 0.3f;
    t.boxes = {{{x0 + dir * s, y}, {2.5f, 1.3f}, c.body[0].heading}};
    c.ahead.push_back(t);
  }
  return c;
}

// One driving up +y from (10, -14): across the way along y = 0.
Traffic::Claim crossing(int priority = 0)
{
  Traffic::Claim c;
  c.priority = priority;
  c.body = {{{10, -14}, {2.5f, 1.3f}, 0.5f * kPi}};
  for (float s = 1; s <= 16; s += 1)
    c.ahead.push_back({s,
                       s / 2 - 0.3f,
                       s / 2 + 0.3f,
                       {{{10, -14 + s}, {2.5f, 1.3f}, 0.5f * kPi}}});
  return c;
}
} // namespace

int main()
{
  TEST("head-on, the same priority: the lower id goes first, the other "
       "keeps out of its reservation")
  {
    Traffic t;
    t.update(1, along(0, 0, 1, 2));
    t.update(2, along(30, 0, -1, 2));
    t.schedule();
    CHECK(t.scheduleOf(1).why == Why::None);
    CHECK(t.scheduleOf(2).why == Why::Reserved);
    CHECK(t.scheduleOf(2).by == 1u);
    CHECK(t.ranksAbove(1, 2));
  }

  TEST("priority ranks first whatever the ids, the same every frame")
  {
    Traffic t;
    t.update(1, along(0, 0, 1, 2, 0));
    t.update(2, along(30, 0, -1, 2, 5));
    for (int frame = 0; frame < 3; ++frame)
    {
      t.schedule();
      CHECK(t.scheduleOf(2).why == Why::None);
      CHECK(t.scheduleOf(1).why == Why::Reserved);
    }
  }

  TEST("a crossing: the lower one stands with its body clear of the "
       "higher's reservation")
  {
    Traffic t;
    t.update(1, along(0, 0, 1, 2)); // along y = 0
    t.update(2, crossing());
    t.schedule();
    const auto b = t.scheduleOf(2);
    CHECK(b.why == Why::Reserved);
    CHECK(!b.inTheWay);
    CHECK(-14 + b.stopAt + 2.5f < -1.3f);
  }

  TEST("never stands in a no-wait place (a gateway)")
  {
    Traffic t;
    t.setNoWait([](glm::vec2 p) { return p.y > -8.0f && p.y < -4.0f; });
    t.update(1, along(0, 0, 1, 2));
    t.update(2, crossing());
    t.schedule();
    const auto b = t.scheduleOf(2);
    CHECK(b.why == Why::Reserved);
    CHECK(-14 + b.stopAt <= -8.0f);
  }

  TEST("already in the higher one's way with nowhere clear: in the way "
       "(a wait spot); the higher one stands for its body")
  {
    Traffic t;
    t.update(1, along(0, 0, 1, 2));
    t.update(2, along(8, 0, -1, 2)); // (nose to nose in an aisle)
    t.schedule();
    CHECK(t.scheduleOf(2).inTheWay);
    CHECK(t.scheduleOf(1).why == Why::Body);
    CHECK(t.scheduleOf(1).by == 2u);
    CHECK(t.reservedAbove(2).size() > 1u);
  }

  TEST("one crossing in front of a higher one that stands for its body "
       "carries on through")
  {
    Traffic t;
    // (Higher, its way along y = 0 through the crossing one's body.)
    t.update(1, along(4, 0, 1, 2, 5));
    Traffic::Claim c;
    c.body = {{{12, 0}, {2.5f, 1.3f}, 0.5f * kPi}};
    for (float s = 1; s <= 16; s += 1)
      c.ahead.push_back({s,
                         s / 2 - 0.3f,
                         s / 2 + 0.3f,
                         {{{12, s}, {2.5f, 1.3f}, 0.5f * kPi}}});
    t.update(2, c);
    t.schedule();
    CHECK(t.scheduleOf(1).why == Why::Body);
    CHECK(t.scheduleOf(2).why == Why::None);
    CHECK(!t.scheduleOf(2).inTheWay);
  }

  TEST("a static obstacle on its way is planned round, whatever its rank")
  {
    Traffic t;
    t.update(1, along(0, 0, 1, 2, 0));
    Traffic::Claim parked;
    parked.priority = 9;
    parked.standing = true;
    parked.solid = true;
    parked.body = {{{10, 0}, {2.5f, 1.3f}, 0.0f}};
    t.update(2, parked);
    t.schedule();
    CHECK(t.scheduleOf(1).why == Why::Solid);
    CHECK(t.scheduleOf(1).by == 2u);
    CHECK(t.scheduleOf(1).stopAt + 2.5f < 10.0f - 2.5f);
  }

  TEST("following the same way at a distance: nobody stands")
  {
    Traffic t;
    t.update(1, along(20, 0, 1, 2));
    t.update(2, along(0, 0, 1, 2));
    t.schedule();
    CHECK(t.scheduleOf(1).why == Why::None);
    // (Only ever short of the one in front's body: a following distance.)
    CHECK(t.scheduleOf(2).why == Why::None ||
          (t.scheduleOf(2).why == Why::Body && t.scheduleOf(2).stopAt > 10));
  }

  TEST("a cooperating pair (cart alongside) is left to keep its own spacing")
  {
    Traffic t;
    t.update(1, along(0, 0, 1, 2));
    t.update(2, along(0, 3, 1, 2));
    t.cooperate(1, 2, true);
    t.schedule();
    CHECK(t.scheduleOf(2).why == Why::None);
  }

  TEST("a narrow place is held by one at a time: the other, higher too, "
       "waits outside it till it's out")
  {
    // A narrow stretch x 10..20 along y = 0; 2 is in it, 1 comes the other
    // way, ranked higher.
    const auto lane = [](float from, float to)
    {
      Traffic::Zone z{from, to, {}};
      return z;
    };
    Traffic t;
    Traffic::Claim in = along(14, 0, 1, 1, 0, 4);
    Traffic::Zone z = lane(0, 6);
    for (float x = 14; x <= 20; x += 1)
      z.area.push_back({{x, 0}, {2.5f, 1.3f}, 0.0f});
    in.zones = {z};
    t.update(2, in);
    t.schedule(); // (2 came first: it holds it)
    Traffic::Claim out = along(36, 0, -1, 2, 5);
    Traffic::Zone w = lane(10, 26);
    for (float x = 26; x >= 10; x -= 1)
      w.area.push_back({{x, 0}, {2.5f, 1.3f}, kPi});
    out.zones = {w};
    t.update(1, out);
    t.schedule();
    CHECK(t.holdsZone(2));
    CHECK(!t.holdsZone(1));
    const auto s1 = t.scheduleOf(1);
    CHECK(s1.why == Why::Zone || s1.why == Why::Body);
    CHECK(36 - s1.stopAt - 2.5f > 20.0f + 2.5f);
    // (2 out of it: 1 is granted it.)
    t.update(2, along(40, 8, 1, 1, 0, 4));
    t.schedule();
    CHECK(!t.holdsZone(2));
    t.schedule();
    CHECK(t.holdsZone(1));
    CHECK(t.scheduleOf(1).why == Why::None);
  }

  TEST("never hold-and-wait: a narrow place it stands short of is given "
       "up")
  {
    // 2 drives +x along y = 0 toward a narrow stretch x 12..30; granted
    // while nothing's in its way.
    Traffic t;
    Traffic::Claim c = along(0, 0, 1, 2);
    Traffic::Zone z{12, 30, {}};
    for (float x = 12; x <= 30; x += 1)
      z.area.push_back({{x, 0}, {2.5f, 1.3f}, 0.0f});
    c.zones = {z};
    t.update(2, c);
    t.schedule();
    CHECK(t.holdsZone(2));
    // A higher one stands across its way at x = 8: it stands short of it,
    // and of the narrow place, which it gives up.
    Traffic::Claim across;
    across.priority = 5;
    across.standing = true;
    across.body = {{{8, 0}, {2.5f, 1.3f}, 0.5f * kPi}};
    t.update(1, across);
    t.update(2, c);
    t.schedule();
    CHECK(t.scheduleOf(2).why != Why::None);
    CHECK(t.scheduleOf(2).stopAt < 12.0f);
    CHECK(!t.holdsZone(2));
  }

  TEST("in a narrow place a higher one waits for, held by its body: the "
       "lower one moves off (no waiting on each other)")
  {
    // 2 (lower) is in a narrow stretch x 0..30 along y = 0 going +x; 1
    // (higher) stands at its far mouth (x 36) to come the other way.
    Traffic t;
    Traffic::Claim in = along(10, 0, 1, 4);
    Traffic::Zone z{0, 20, {}};
    for (float x = 10; x <= 30; x += 1)
      z.area.push_back({{x, 0}, {2.5f, 1.3f}, 0.0f});
    in.zones = {z};
    t.update(2, in);
    t.schedule();
    CHECK(t.holdsZone(2));
    Traffic::Claim out = along(36, 0, -1, 2, 5);
    Traffic::Zone w{1, 16, {}};
    for (float x = 35; x >= 20; x -= 1)
      w.area.push_back({{x, 0}, {2.5f, 1.3f}, kPi});
    out.zones = {w};
    t.update(1, out);
    t.update(2, in);
    for (int frame = 0; frame < 2; ++frame)
      t.schedule();
    CHECK(t.scheduleOf(1).why == Why::Zone);
    CHECK(t.scheduleOf(1).by == 2);
    // (Not held by 1's reservation (it waits for 2), only by its body: 2 is
    // in its way, to a wait spot, never both standing for good.)
    const auto s2 = t.scheduleOf(2);
    CHECK(s2.why == Why::Body);
    CHECK(s2.inTheWay);
  }

  TEST("the wait-for graph: each cycle found once, from its lowest id; "
       "chains and self-waits are no cycle")
  {
    const auto cycles = Traffic::cycles(
        {{3, 1}, {1, 2}, {2, 3}, {4, 1}, {5, 6}, {7, 8}, {8, 7}, {9, 9}});
    CHECK(cycles.size() == 2u);
    if (cycles.size() == 2u)
    {
      CHECK(cycles[0] == std::vector<Traffic::AgentId>({1, 2, 3}));
      CHECK(cycles[1] == std::vector<Traffic::AgentId>({7, 8}));
    }
    CHECK(Traffic::cycles({{1, 2}, {2, 3}}).empty());
  }

  TEST("waitsOn: one held still short of another is its edge; one moving or "
       "slowing only has none")
  {
    Traffic t;
    t.update(1, along(0, 0, 1, 2));
    Traffic::Claim held = along(30, 0, -1, 2);
    held.still = true;
    t.update(2, held);
    t.schedule();
    // (Its stop is ahead of it, not where it stands: slowing, not held.)
    CHECK(t.scheduleOf(2).why == Why::Reserved);
    const bool atStop = t.scheduleOf(2).stopAt <= held.s + 0.5f;
    CHECK(t.waitsOn(2) == (atStop ? 1u : 0u));
    CHECK(t.waitsOn(1) == 0u);
    Traffic::Claim moving = along(30, 0, -1, 2);
    t.update(2, moving);
    t.schedule();
    CHECK(t.waitsOn(2) == 0u);
  }

  TEST("a crossing the higher one will have cleared in time: the lower one "
       "eases off to reach its stop as it clears, never past its stop")
  {
    Traffic t;
    // (Higher, along x at 2 m/s through x 10; lower along y at 2 m/s
    // through y 0, 14 m off: it would be there as the higher one is.)
    t.update(1, along(0, 0, 1, 2, 5));
    t.update(2, crossing()); // (its stop is short of the crossing)
    t.schedule();
    const auto s = t.scheduleOf(2);
    CHECK(s.why == Why::Reserved);
    CHECK(s.by == 1u);
    CHECK(std::isfinite(s.easeTo));
    CHECK(s.easeTo > 0.0f);
    CHECK(s.stopAt < 14.0f);
    // (The higher one standing on the crossing: no telling when it's clear,
    // so no easing.)
    Traffic::Claim stands = along(0, 0, 1, 2, 5);
    stands.still = true;
    stands.standing = true;
    stands.body = {{{10, 0}, {2.5f, 1.3f}, 0.0f}};
    stands.ahead.clear();
    t.update(1, stands);
    t.update(2, crossing());
    t.schedule();
    CHECK(!std::isfinite(t.scheduleOf(2).easeTo));
  }

  TEST("standing touching one standing still: its way out of the touch "
       "isn't held by that body (the planner's let-off, kEscapeReach), its "
       "way into it further on still is")
  {
    // (Its front 1.5 m into the back of a solid one; driving away from it,
    // along -x, still in it its first metre: let off.)
    Traffic::Claim stood;
    stood.priority = 9;
    stood.standing = true;
    stood.solid = true;
    stood.body = {{{3.5f, 0}, {2.5f, 1.3f}, 0.0f}};
    {
      Traffic t;
      Traffic::Claim out = along(0, 0, -1, 2);
      out.still = true;
      t.update(1, out);
      t.update(2, stood);
      t.schedule();
      CHECK(t.scheduleOf(1).why == Why::None);
      CHECK(t.waitsOn(1) == 0u);
    }
    // (Its way on into it, past the let-off: held, planned round.)
    {
      Traffic t;
      Traffic::Claim into = along(0, 0, 1, 2);
      into.still = true;
      t.update(1, into);
      t.update(2, stood);
      t.schedule();
      CHECK(t.scheduleOf(1).why == Why::Solid);
    }
    // (Moving, never let off: one passing close is held as ever.)
    {
      Traffic t;
      t.update(1, along(0, 0, -1, 2));
      t.update(2, stood);
      t.schedule();
      CHECK(t.scheduleOf(1).why == Why::Solid);
    }
  }

  TEST("half across a higher one's way when its reservation comes: on its "
       "way out it's never held standing in its way; it goes on out and "
       "the higher one stands short of its way out")
  {
    // (The higher one along y = 0; the lower one moving up +y at x = 10,
    // its body over the higher one's way.)
    const auto halfAcross = []
    {
      Traffic::Claim c;
      c.body = {{{10, -0.5f}, {2.5f, 1.3f}, 0.5f * kPi}};
      for (float s = 0; s <= 10; s += 1)
        c.ahead.push_back({s,
                           s / 2 - 0.3f,
                           s / 2 + 0.3f,
                           {{{10, -0.5f + s}, {2.5f, 1.3f}, 0.5f * kPi}}});
      return c;
    };
    Traffic t;
    t.update(1, along(0, 0, 1, 2, 5));
    t.update(2, halfAcross());
    t.schedule();
    CHECK(t.scheduleOf(2).why == Why::None);
    CHECK(!t.scheduleOf(2).inTheWay);
    CHECK(t.scheduleOf(1).why == Why::Body);
    CHECK(t.scheduleOf(1).by == 2u);
    CHECK(t.scheduleOf(1).stopAt + 2.5f < 10.0f - 1.3f);
  }

  TEST("a way planned for a lower one, checked against the higher ones' "
       "reservations as planned: one with a clear place to wait short of "
       "them isn't crossing; one that would be held in a higher one's way "
       "is, and whose")
  {
    Traffic t;
    t.update(1, along(0, 0, 1, 2, 5)); // along y = 0
    // (Up +y from y = -14, across y = 0: it can wait short of it.)
    Traffic::Claim way = crossing();
    way.still = true;
    CHECK(t.crossing(2, way).at < 0);
    // (Standing in its way, not leaving it: held there at once.)
    Traffic::Claim in;
    in.still = true;
    in.body = {{{10, 0}, {2.5f, 1.3f}, 0.5f * kPi}};
    for (float s = 0; s <= 12; s += 1)
      in.ahead.push_back({s,
                          s / 2 - 0.3f,
                          s / 2 + 0.3f,
                          {{{10, s}, {2.5f, 1.3f}, 0.5f * kPi}}});
    t.update(2, in); // (ranked as it is)
    const Traffic::Crossing x = t.crossing(2, in);
    CHECK(x.at >= 0);
    CHECK(x.by == 1u);
    // (Leaving it for a wait spot: its way out doesn't hold it...)
    in.leaving = 1;
    CHECK(t.crossing(2, in).at < 0);
    // (...but another higher one's way it would run into, with nowhere
    // clear between the two, does.)
    t.update(3, along(4, 5.5f, 1, 2, 4));
    const Traffic::Crossing y = t.crossing(2, in);
    CHECK(y.at > 0);
    CHECK(y.by == 3u);
    // (A lower one's way never holds it.)
    t.update(3, along(4, 5.5f, 1, 2, -1));
    CHECK(t.crossing(2, in).at < 0);
    // (Clear between the two: a trip waits there; a way to a wait spot
    // waits only where it starts, so it's crossing.)
    t.update(3, along(4, 10.0f, 1, 2, 4));
    CHECK(t.crossing(2, in).at < 0);
    const Traffic::Crossing z = t.crossing(2, in, false);
    CHECK(z.at > 0);
    CHECK(z.by == 3u);
  }

  return testing::report("trafficTests");
}
