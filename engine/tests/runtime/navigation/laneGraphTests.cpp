#include "../../testHarness.h"

#include <engine/runtime/navigation/laneGraph.h>
#include <engine/runtime/navigation/vehicle/laneTrip.h>

#include <chrono>
#include <cstdio>

using namespace sfs::nav;
using namespace sfs::nav::vehicle;

namespace
{
// A straight line of points `step` apart from a to b.
std::vector<glm::vec2> line(glm::vec2 a, glm::vec2 b, float step = 1.0f)
{
  const int n =
      std::max(1, static_cast<int>(std::ceil(glm::distance(a, b) / step)));
  std::vector<glm::vec2> out;
  for (int k = 0; k <= n; ++k)
    out.push_back(
        glm::mix(a, b, static_cast<float>(k) / static_cast<float>(n)));
  return out;
}
// Lanes both ways between two nodes, each `side` m to the left of the middle.
void road(LaneGraph& g,
          int a,
          int b,
          float side,
          float speed = 10.0f,
          uint32_t tag = 0,
          float cost = 1.0f)
{
  const glm::vec2 pa = g.nodes()[static_cast<std::size_t>(a)],
                  pb = g.nodes()[static_cast<std::size_t>(b)];
  const glm::vec2 d = glm::normalize(pb - pa), left(-d.y, d.x);
  g.addLane({a, b, line(pa + left * side, pb + left * side), speed, cost, tag});
  g.addLane({b, a, line(pb - left * side, pa - left * side), speed, cost, tag});
}
float headingOf(const LaneGraph::Stretch& s)
{
  return s.poses[s.poses.size() / 2].heading;
}
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
} // namespace

int main()
{
  TEST("a trip along one road should keep to the lane going its way")
  {
    LaneGraph g;
    const int a = g.addNode({0, 0}), b = g.addNode({400, 0});
    road(g, a, b, 1.5f);
    LaneGraph::Options o;
    const auto east = g.route({20, -10}, {380, -10}, o);
    CHECK(east.has_value());
    if (east)
    {
      CHECK(east->size() == 1u);
      // (Going east, on the left: y = +1.5.)
      CHECK(std::abs(east->front().poses[10].position.y - 1.5f) < 0.01f);
      CHECK(std::abs(headingOf(east->front())) < 0.01f);
      // (Starting `lead` on from where it gets on, ending `lead` short.)
      CHECK(std::abs(east->front().poses.front().position.x -
                     (20.0f + o.lead)) < 0.6f);
      CHECK(std::abs(east->front().poses.back().position.x -
                     (380.0f - o.lead)) < 0.6f);
    }
    const auto west = g.route({380, 10}, {20, 10}, o);
    CHECK(west.has_value());
    if (west)
      CHECK(std::abs(west->front().poses[10].position.y + 1.5f) < 0.01f);
  }

  TEST("the cheapest way through the network should be taken (A*)")
  {
    // A square: west-south-east the long way, a cheap diagonal across.
    LaneGraph g;
    const int sw = g.addNode({0, 0}), se = g.addNode({200, 0}),
              ne = g.addNode({200, 200}), nw = g.addNode({0, 200});
    road(g, sw, se, 0.0f, 10.0f, 1);
    road(g, se, ne, 0.0f, 10.0f, 2);
    road(g, sw, nw, 0.0f, 10.0f, 3);
    road(g, nw, ne, 0.0f, 10.0f, 4);
    LaneGraph::Options o;
    const auto r = g.route({100, -5}, {205, 100}, o);
    CHECK(r.has_value());
    if (r)
    {
      std::vector<uint32_t> tags;
      for (const auto& s : *r)
        tags.insert(tags.end(), s.tags.begin(), s.tags.end());
      CHECK(tags.size() == 2u);
      if (tags.size() == 2)
      {
        CHECK(tags[0] == 1u);
        CHECK(tags[1] == 2u);
      }
      // (The corner at the south-east: two stretches, each cut back for it.)
      CHECK(r->size() == 2u);
      if (r->size() == 2)
      {
        CHECK(r->front().poses.back().position.x < 200.0f - o.radius);
        CHECK(r->back().poses.front().position.y > o.radius);
      }
    }
    // A dearer lane is avoided: the south road at 3x cost sends it round the
    // north (to a point on the east road nearer the north-east corner).
    LaneGraph h;
    const int sw2 = h.addNode({0, 0}), se2 = h.addNode({200, 0}),
              ne2 = h.addNode({200, 200}), nw2 = h.addNode({0, 200});
    road(h, sw2, se2, 0.0f, 10.0f, 1, 3.0f);
    road(h, se2, ne2, 0.0f, 10.0f, 2);
    road(h, sw2, nw2, 0.0f, 10.0f, 3);
    road(h, nw2, ne2, 0.0f, 10.0f, 4);
    const auto round = h.route({-5, 20}, {205, 120}, o);
    CHECK(round.has_value());
    if (round)
      CHECK(round->front().tags.front() == 3u);
  }

  TEST("a trip not worth taking the lanes for (or out of reach) should be none")
  {
    LaneGraph g;
    const int a = g.addNode({0, 0}), b = g.addNode({400, 0});
    road(g, a, b, 1.5f);
    LaneGraph::Options o;
    CHECK(!g.route({100, -25}, {130, -25}, o)
               .has_value()); // (straight there's quicker)
    CHECK(!g.route({100, -60}, {300, -10}, o)
               .has_value()); // (start out of reach)
    LaneGraph empty;
    CHECK(!empty.route({0, 0}, {100, 0}, o).has_value());
    // A one-way lane the wrong way: no way through.
    LaneGraph one;
    const int c = one.addNode({0, 0}), d = one.addNode({400, 0});
    one.addLane({c, d, line({0, 0}, {400, 0}), 10.0f, 1.0f, 0});
    CHECK(one.route({20, 5}, {380, 5}, o).has_value());
    CHECK(!one.route({380, 5}, {20, 5}, o).has_value());
  }

  TEST("a big network should be searched in milliseconds")
  {
    // A 40 x 40 grid of roads 50 m apart (3120 lanes each way).
    LaneGraph g;
    const int n = 40;
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x)
        g.addNode({50.0f * x, 50.0f * y});
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x)
      {
        if (x + 1 < n)
          road(g, y * n + x, y * n + x + 1, 1.5f);
        if (y + 1 < n)
          road(g, y * n + x, (y + 1) * n + x, 1.5f);
      }
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = g.route({10, -5}, {1940, 1955}, LaneGraph::Options());
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    std::printf("  40x40 grid: %.2f ms\n", ms);
    CHECK(r.has_value());
    CHECK(ms < 100.0);
  }

  TEST("a lane trip should join its stretches with searched windows, one model")
  {
    LaneGraph g;
    const int a = g.addNode({0, 0}), b = g.addNode({200, 0}),
              c = g.addNode({200, 200});
    road(g, a, b, 1.5f);
    road(g, b, c, 1.5f);
    LaneTripRequest q;
    q.plan.vehicle = tractor();
    q.plan.space = std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{});
    q.plan.from = {{10, -6}, 0.0f};
    q.plan.goals = {{{{206, 180}, 1.5708f}}};
    q.lanes.radius = 6.0f;
    q.lanes.lead = 1.6f * 6.0f + 4.0f;
    int windows = 0;
    q.search = [&](const PlanRequest& r, LaneTripRequest::Window)
    {
      ++windows;
      return planNow(r);
    };
    const auto trip = planLaneTrip(g, q);
    CHECK(trip.has_value());
    if (trip)
    {
      CHECK(trip->state == PlanState::Found);
      CHECK(trip->stretches == 2);
      // (On, the corner, off.)
      CHECK(windows == 3);
      CHECK(trip->parts.size() == 5u);
      if (trip->parts.size() == 5)
      {
        CHECK(trip->parts[0].window == LaneTripRequest::Window::On);
        CHECK(trip->parts[2].window == LaneTripRequest::Window::Corner);
        CHECK(trip->parts[4].window == LaneTripRequest::Window::Off);
        // (Each window ends where the next part starts.)
        CHECK(glm::distance(trip->parts[0].found.path.end().position,
                            trip->parts[1].stretch.poses.front().position) <
              0.1f);
        CHECK(glm::distance(trip->parts[4].found.path.end().position,
                            glm::vec2(206, 180)) < 0.1f);
        // (A corner is driven forward.)
        CHECK(trip->parts[2].found.path.cusps() == 0);
      }
      CHECK(trip->laneMetres > 300.0f);
      std::printf("  trip: graph %.2f ms, windows %.0f ms\n",
                  trip->graphMs,
                  trip->windowMs);
    }
    // A window with no way ends the trip there (the parts before it good).
    LaneTripRequest blocked = q;
    blocked.search = {};
    blocked.plan.space =
        std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{
            {{206, 180}, {3.0f, 3.0f}, 0.0f, 0.0f, 2.0f, false, 7}});
    const auto stopped = planLaneTrip(g, blocked);
    CHECK(stopped.has_value());
    if (stopped)
    {
      CHECK(stopped->state == PlanState::Blocked);
      CHECK(stopped->parts.back().window == LaneTripRequest::Window::Off);
    }
    // No lanes worth taking: none (planned as any other way).
    LaneTripRequest near = q;
    near.plan.goals = {{{{20, -6}, 0.0f}}};
    CHECK(!planLaneTrip(g, near).has_value());
  }

  TEST("a lane passing too near a collider: the trip leaves the lane round "
       "it (a Round window), no stretch it hands back touches anything, and "
       "planning it again never gives the blocked way")
  {
    LaneGraph g;
    const int a = g.addNode({0, 0}), b = g.addNode({200, 0}),
              c = g.addNode({200, 200});
    road(g, a, b, 1.5f);
    road(g, b, c, 1.5f);
    LaneTripRequest q;
    q.plan.vehicle = tractor();
    // (A tree beside the lane going east: its trunk 0.1 m into the rig's
    // side as the rig drives the lane's line.)
    q.plan.space = std::make_shared<BoxSpace>(std::vector<BoxSpace::Solid>{
        {{100.0f, 3.2f}, {0.5f, 0.5f}, 0.0f, 0.0f, 4.0f, false, 38}});
    q.plan.from = {{10, -6}, 0.0f};
    q.plan.goals = {{{{206, 180}, 1.5708f}}};
    q.lanes.radius = 6.0f;
    q.lanes.lead = 1.6f * 6.0f + 4.0f;
    // (The lane line itself does touch it.)
    {
      std::vector<Segment> straight{{1, 40.0f, 0.0f, 0.0f}};
      CHECK(touchesAlong(q.plan, {{80.0f, 1.5f}, 0.0f}, 0.0f, straight)
                .has_value());
    }
    const auto trip = planLaneTrip(g, q);
    CHECK(trip.has_value());
    if (trip)
    {
      CHECK(trip->state == PlanState::Found);
      CHECK(trip->rounds >= 1);
      int rounds = 0;
      for (const LaneTrip::Part& part : trip->parts)
      {
        if (part.window)
        {
          rounds += *part.window == LaneTripRequest::Window::Round;
          if (*part.window == LaneTripRequest::Window::Round)
            CHECK(part.found.path.cusps() == 0); // (forward round it)
          continue;
        }
        // Each stretch checked as it's driven: clear.
        std::vector<Segment> steps;
        const auto& p = part.stretch.poses;
        for (std::size_t k = 1; k < p.size(); ++k)
        {
          const float ds = glm::distance(p[k].position, p[k - 1].position);
          const float k0 = wrapAngle(p[k].heading - p[k - 1].heading) / ds;
          steps.push_back({1, ds, k0, k0});
        }
        CHECK(!touchesAlong(q.plan, p.front(), 0.0f, steps).has_value());
      }
      CHECK(rounds == trip->rounds);
      // The same again: the same trip, never the blocked way.
      const auto again = planLaneTrip(g, q);
      CHECK(again.has_value() && again->parts.size() == trip->parts.size() &&
            again->state == PlanState::Found);
      std::printf("  round the tree: %d round, %d stretches, windows %.0f ms\n",
                  trip->rounds,
                  trip->stretches,
                  trip->windowMs);
    }
  }

  return testing::report("laneGraphTests");
}
