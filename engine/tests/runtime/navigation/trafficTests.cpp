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

  return testing::report("trafficTests");
}
