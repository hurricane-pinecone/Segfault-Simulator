#include "engine/runtime/navigation/vehicle/laneTrip.h"

#include "engine/runtime/navigation/vehicle/steering.h"

#include <algorithm>
#include <chrono>
#include <deque>

namespace sfs::nav::vehicle
{

namespace
{
double msSince(std::chrono::steady_clock::time_point t0)
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

// A stretch as it's driven (drivenSteps(): forward, each step its poses'
// distance apart; a game lays its route's points from the poses, and its
// look-ahead checks them, the same way).
std::vector<Segment> stepsOf(const std::vector<Pose2>& poses)
{
  std::vector<float> along{0.0f};
  for (std::size_t k = 1; k < poses.size(); ++k)
    along.push_back(along.back() +
                    glm::distance(poses[k].position, poses[k - 1].position));
  return drivenSteps(poses, along, std::vector<int>(poses.size(), 1));
}

// Its trailer's angle at the end of the first `count` steps, from `hitch`.
float swungAlong(const VehicleModel& v,
                 float hitch,
                 const std::vector<Segment>& steps,
                 std::size_t count)
{
  if (!v.trailer)
    return 0.0f;
  for (std::size_t k = 0; k < std::min(count, steps.size()); ++k)
    hitch = hitchStep(*v.trailer,
                      hitch,
                      static_cast<float>(steps[k].gear) * steps[k].length,
                      steps[k].k0);
  return hitch;
}

// Poses a..b (inclusive) of a stretch.
LaneGraph::Stretch
slice(const LaneGraph::Stretch& s, std::size_t a, std::size_t b)
{
  LaneGraph::Stretch out;
  out.speed = s.speed;
  out.tags = s.tags;
  out.poses.assign(s.poses.begin() + static_cast<std::ptrdiff_t>(a),
                   s.poses.begin() + static_cast<std::ptrdiff_t>(b + 1));
  out.curvature.assign(
      s.curvature.begin() + static_cast<std::ptrdiff_t>(a),
      s.curvature.begin() + static_cast<std::ptrdiff_t>(b + 1));
  return out;
}

// A stretch's poses' distances along it from its first.
std::vector<float> alongOf(const LaneGraph::Stretch& s)
{
  std::vector<float> out{0.0f};
  for (std::size_t k = 1; k < s.poses.size(); ++k)
    out.push_back(out.back() +
                  glm::distance(s.poses[k].position, s.poses[k - 1].position));
  return out;
}

// (Rounds one stretch may take at most: past that, the rest of it is left to
// the next window.)
constexpr int kMostRounds = 16;
} // namespace

std::optional<LaneTrip> planLaneTrip(const LaneGraph& graph,
                                     const LaneTripRequest& request)
{
  using Window = LaneTripRequest::Window;
  const PlanRequest& q = request.plan;
  if (q.goals.empty())
    return std::nullopt;
  const auto t0 = std::chrono::steady_clock::now();
  const std::optional<std::vector<LaneGraph::Stretch>> way = graph.route(
      q.from.position, q.goals.front().pose.position, request.lanes);
  LaneTrip trip;
  trip.graphMs = msSince(t0);
  if (!way)
    return std::nullopt;
  const auto search = [&](const PlanRequest& r, Window w)
  {
    const auto started = std::chrono::steady_clock::now();
    PlanResult out = request.search ? request.search(r, w) : planNow(r);
    trip.windowMs += msSince(started);
    return out;
  };
  Pose2 at = q.from;
  float hitch = q.hitch;
  // A window from where it is to `goals`: true if found (and `at` on).
  const auto window = [&](std::vector<Goal> goals, bool reverse, Window kind)
  {
    PlanRequest r = q;
    r.from = at;
    r.hitch = hitch;
    r.goals = std::move(goals);
    r.vehicle.mayReverse = q.vehicle.mayReverse && reverse;
    r.tunables.improveNodes =
        std::min(r.tunables.improveNodes, request.windowImprove);
    LaneTrip::Part part;
    part.window = kind;
    part.to = r.goals.front().pose;
    part.found = search(r, kind);
    const bool found = part.found.state == PlanState::Found;
    if (found && !part.found.path.empty())
    {
      at = part.found.path.end();
      hitch = part.found.path.points.back().hitch;
    }
    part.hitch = hitch;
    trip.state = part.found.state;
    trip.parts.push_back(std::move(part));
    return found;
  };
  // A stretch (or what's left of one) driven: its part, and on from its end.
  const auto drive = [&](LaneGraph::Stretch stretch, float swung)
  {
    const std::vector<float> along = alongOf(stretch);
    trip.laneMetres += along.back();
    at = stretch.poses.back();
    hitch = swung;
    LaneTrip::Part part;
    part.stretch = std::move(stretch);
    part.hitch = swung;
    trip.parts.push_back(std::move(part));
    ++trip.stretches;
  };
  for (std::size_t s = 0; s < way->size(); ++s)
  {
    std::deque<LaneGraph::Stretch> pieces{(*way)[s]};
    Window kind = s == 0 ? Window::On : Window::Corner;
    int rounds = 0;
    while (!pieces.empty())
    {
      LaneGraph::Stretch piece = std::move(pieces.front());
      pieces.pop_front();
      if (!window({Goal{piece.poses.front()}},
                  kind == Window::On && request.reverseOn,
                  kind))
        return trip;
      kind = Window::Round;
      // The stretch as it's driven, checked whole by THE check of a laid way
      // (its trailer swung from where the window left it).
      const std::vector<Segment> steps = stepsOf(piece.poses);
      const std::optional<float> touch =
          q.space ? touchesAlong(q, piece.poses.front(), hitch, steps)
                  : std::nullopt;
      if (!touch)
      {
        const float swung = swungAlong(q.vehicle, hitch, steps, steps.size());
        drive(std::move(piece), swung);
        continue;
      }
      // It touches `touch` m on: driven up to a lead short of it, a window
      // round it back onto the lane a lead past it (where the rig stands
      // clear), the rest checked the same.
      const std::vector<float> along = alongOf(piece);
      const float lead = std::max(request.lanes.lead, 1.0f);
      std::size_t keep = 0;
      while (keep + 1 < along.size() && along[keep + 1] <= *touch - lead)
        ++keep;
      std::size_t resume = 0;
      for (std::size_t k = 0; k < along.size() && !resume; ++k)
        if (along[k] >= *touch + lead && k > keep &&
            !touches(q, piece.poses[k], 0.0f))
          resume = k;
      ++trip.rounds;
      if (keep > 0 && along[keep] >= request.lanes.least)
      {
        // (A step per pose after the first: a lane's poses are evenly
        // spaced, none dropped.)
        const float swung = swungAlong(q.vehicle, hitch, steps, keep);
        drive(slice(piece, 0, keep), swung);
      }
      if (resume > 0 && along.back() - along[resume] >= request.lanes.least &&
          ++rounds <= kMostRounds)
        pieces.push_front(slice(piece, resume, piece.poses.size() - 1));
    }
  }
  window(q.goals, request.reverseOff, Window::Off);
  return trip;
}

} // namespace sfs::nav::vehicle
