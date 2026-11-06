#include "../../testHarness.h"

#include <engine/runtime/navigation/traffic.h>

using namespace sfs::nav;

namespace
{
// An agent driving along +x or -x from x0 at `speed`: its claim for `ahead`
// seconds, a 5 x 2.6 m footprint a metre apart.
Traffic::Claim along(float x0,
                     float y,
                     float dir,
                     float speed,
                     int priority = 0,
                     float ahead = 8.0f,
                     uint64_t path = 1)
{
  Traffic::Claim c;
  c.priority = priority;
  c.path = path;
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
} // namespace

int main()
{
  TEST("two vehicles meeting head-on: only the later one gives way")
  {
    Traffic t;
    t.update(1, along(0, 0, 1, 2), 0.0);
    t.update(2, along(30, 0, -1, 2), 1.0); // (claimed a second later)
    const auto a = t.advise(1), b = t.advise(2);
    CHECK(a.kind == Traffic::Advice::Kind::Go);
    CHECK(b.kind == Traffic::Advice::Kind::HoldAt ||
          b.kind == Traffic::Advice::Kind::ClearOut);
    CHECK(b.other == 1u);
  }

  TEST("a working machine should have the right of way whoever claimed first")
  {
    Traffic t;
    t.update(1, along(0, 0, 1, 2, 0), 0.0);
    t.update(2, along(30, 0, -1, 2, 5), 1.0);
    CHECK(t.advise(2).kind == Traffic::Advice::Kind::Go);
    CHECK(t.advise(1).kind != Traffic::Advice::Kind::Go);
  }

  TEST("a crossing: the one giving way holds short of the other's way")
  {
    Traffic t;
    t.update(1, along(0, 0, 1, 2), 0.0); // along y = 0
    Traffic::Claim c;
    c.path = 1;
    c.body = {{{10, -14}, {2.5f, 1.3f}, 0.5f * kPi}};
    for (float s = 1; s <= 16; s += 1)
      c.ahead.push_back({s,
                         s / 2 - 0.3f,
                         s / 2 + 0.3f,
                         {{{10, -14 + s}, {2.5f, 1.3f}, 0.5f * kPi}}});
    t.update(2, c, 1.0);
    const auto b = t.advise(2);
    CHECK(b.kind == Traffic::Advice::Kind::HoldAt);
    // (Stopped with its whole body clear of the other's claim: y + 2.5 < -1.3 -
    // margin.)
    CHECK(-14 + b.s + 2.5f < -1.3f);
  }

  TEST("never told to wait in a gateway")
  {
    Traffic t;
    t.setNoWait(
        [](glm::vec2 p)
        { return p.y > -8.0f && p.y < -4.0f; }); // (a gate across its way)
    t.update(1, along(0, 0, 1, 2), 0.0);
    Traffic::Claim c;
    c.path = 1;
    c.body = {{{10, -14}, {2.5f, 1.3f}, 0.5f * kPi}};
    for (float s = 1; s <= 16; s += 1)
      c.ahead.push_back({s,
                         s / 2 - 0.3f,
                         s / 2 + 0.3f,
                         {{{10, -14 + s}, {2.5f, 1.3f}, 0.5f * kPi}}});
    t.update(2, c, 1.0);
    const auto b = t.advise(2);
    CHECK(b.kind == Traffic::Advice::Kind::HoldAt);
    CHECK(-14 + b.s <= -8.0f);
  }

  TEST("already in the other's way: clear out; the other waits for its body")
  {
    Traffic t;
    t.update(1, along(0, 0, 1, 2), 0.0);
    t.update(2, along(8, 0, -1, 2), 1.0); // (nose to nose in an aisle)
    CHECK(t.advise(2).kind == Traffic::Advice::Kind::ClearOut);
    CHECK(t.advise(1).kind == Traffic::Advice::Kind::Wait);
  }

  TEST("a yielder that can't clear swaps once; both stuck is a deadlock")
  {
    Traffic t;
    t.update(1, along(0, 0, 1, 2), 0.0);
    t.update(2, along(8, 0, -1, 2), 1.0);
    t.cannotClear(2, 1);
    CHECK(t.advise(1).kind == Traffic::Advice::Kind::ClearOut);
    CHECK(t.deadlocks().empty());
    t.cannotClear(1, 2);
    CHECK(t.deadlocks().size() == 1u);
  }

  TEST("following the same way at a distance is no meeting to give way at")
  {
    Traffic t;
    t.update(1, along(20, 0, 1, 2), 0.0);
    t.update(2, along(0, 0, 1, 2), 1.0);
    CHECK(t.advise(1).kind == Traffic::Advice::Kind::Go);
    CHECK(t.advise(2).kind == Traffic::Advice::Kind::Go);
  }

  TEST("a cooperating pair (cart alongside) is left to keep its own spacing")
  {
    Traffic t;
    t.update(1, along(0, 0, 1, 2), 0.0);
    t.update(2, along(0, 3, 1, 2), 1.0);
    t.cooperate(1, 2, true);
    CHECK(t.advise(2).kind == Traffic::Advice::Kind::Go);
  }

  return testing::report("trafficTests");
}
