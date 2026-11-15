#include "engine/runtime/navigation/vehicle/planner.h"

#include "engine/core/util/cpuTiming.h"
#include "engine/runtime/navigation/vehicle/smoother.h"
#include "engine/runtime/navigation/vehicle/speedProfile.h"
#include "engine/runtime/navigation/vehicle/vehicleSearch.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sfs::nav::vehicle
{

void addHybridAStar(Searches& searches); // (hybridAStar.cpp)
void takeProfTimes(double& heur,
                   double& shot,
                   double& heading,
                   double& words,
                   double& near); // (where the time goes: PlanStats)

Searches& searches()
{
  static Searches* s = []
  {
    auto* list = new Searches;
    addHybridAStar(*list);
    return list;
  }();
  return *s;
}

namespace
{
double msSince(std::chrono::steady_clock::time_point t0)
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

// Laid as it'll be driven: sampled, checked exactly, its speed profile.
void finish(const Motions& rig,
            const PlanRequest& q,
            std::vector<Segment> segs,
            PlanResult& r)
{
  r.path = {};
  r.path.start = q.from;
  r.path.startHitch = q.hitch;
  r.path.segments = std::move(segs);
  sample(r.path, 0.05f, q.vehicle.trailer ? &*q.vehicle.trailer : nullptr);
  Motions::Walk w;
  w.hitch = q.hitch;
  w.escape = rig.standsClear(q.from, q.hitch) ? 0.0f : kEscapeReach;
  w.slack = 0.06f; // (the trailer's angle integrated finer than the search did)
  // Checked as it's driven: its samples' steps (drivenSteps()), the very steps
  // a game checks the points it's handed (its look-ahead) and a lane trip
  // checks a stretch by. Its segments walked a step apart put the poses (and a
  // trailer's angle, integrated over other steps) a little elsewhere: a way
  // backing a trailer past a tree by a hair was clear by its segments and
  // touching by its points, planned and refused again for ever (D3e, Paddock
  // Gum 4).
  const std::vector<Segment> driven = drivenSteps(r.path);
  const auto te = std::chrono::steady_clock::now();
  const bool clear =
      rig.walk(q.from, driven, q.tunables.exactStep, true, w, true);
  r.stats.exactMs = msSince(te);
  if (!clear)
  {
    r.stats.exactMiss = true;
    if (w.hit)
      r.why = "the exact check touches " + std::to_string(w.hit->id) + " at (" +
              std::to_string(w.hit->point.x) + ", " +
              std::to_string(w.hit->point.y) + ")";
    else
      r.why = "the exact check fails at (" + std::to_string(w.end.position.x) +
              ", " + std::to_string(w.end.position.y) +
              ") (swept, or the trailer's angle)";
  }
  const auto tp = std::chrono::steady_clock::now();
  profile(r.path, q.vehicle, q.speedCap);
  r.stats.profileMs = msSince(tp);
  r.seconds = r.path.duration();
}
} // namespace

float priceOf(const PlanRequest& request,
              Pose2 from,
              float hitch,
              const std::vector<Segment>& segments,
              bool check)
{
  const Motions rig(request, false);
  Motions::Walk w;
  w.hitch = hitch;
  w.escape = check && !rig.standsClear(from, hitch) ? kEscapeReach : 0.0f;
  if (!rig.walk(from, segments, request.tunables.checkStep, check, w))
    return kInf;
  return w.cost;
}

std::optional<Space::Hit>
touches(const PlanRequest& request, Pose2 pose, std::optional<float> hitch)
{
  if (!request.space)
    return std::nullopt;
  return Motions(request, false).touches(pose, hitch);
}

std::optional<float> touchesAlong(const PlanRequest& request,
                                  Pose2 from,
                                  float hitch,
                                  std::span<const Segment> segments,
                                  bool endIn)
{
  if (!request.space || segments.empty())
    return std::nullopt;
  // (Either gear: the way's the game's, the check only says what it touches.)
  PlanRequest q = request;
  q.vehicle.mayReverse = true;
  for (VehicleModel::Mode& m : q.vehicle.modes)
    m.forwardOnly = false;
  const Motions rig(q, false);
  return touchesAlong(rig, from, hitch, segments, endIn, q.tunables.exactStep);
}

std::optional<float> touchesAlong(const Motions& rig,
                                  Pose2 from,
                                  float hitch,
                                  std::span<const Segment> segments,
                                  bool endIn,
                                  float step)
{
  if (segments.empty())
    return std::nullopt;
  step = std::max(step, 0.01f);
  std::vector<Segment> segs(segments.begin(), segments.end());
  // Ending against what it's to come to: its last metres let off (cut).
  if (endIn && rig.touches(endOf(from, segs), std::nullopt))
  {
    float keep = std::max(lengthOf(segs) - kEscapeReach, 0.0f);
    std::vector<Segment> cut;
    for (const Segment& s : segs)
    {
      if (keep <= 0.0f)
        break;
      Segment c = s;
      if (s.length > keep)
      {
        c.length = keep;
        c.k1 = s.k0 + (s.k1 - s.k0) * keep / std::max(s.length, 1e-6f);
      }
      keep -= c.length;
      cut.push_back(c);
    }
    segs = std::move(cut);
  }
  Motions::Walk w;
  w.hitch = hitch;
  w.escape = rig.standsClear(from, hitch) ? 0.0f : kEscapeReach;
  w.slack = 0.06f; // (as the final check: finer than the search's)
  {
    Motions::Walk all = w;
    if (rig.walk(from, segs, step, true, all))
      return std::nullopt;
  }
  // Where: a step at a time, the walk carried on from one to the next.
  float s = 0.0f;
  Pose2 at = from;
  for (const Segment& seg : segs)
  {
    if (seg.length <= 0.0f)
      continue;
    const int n =
        std::max(1, static_cast<int>(std::ceil(seg.length / step - 1e-4f)));
    const float ds = seg.length / static_cast<float>(n);
    for (int i = 0; i < n; ++i)
    {
      const float k0 =
          seg.k0 + (seg.k1 - seg.k0) * ds * static_cast<float>(i) / seg.length;
      const Segment part{
          seg.gear, ds, k0, k0 + (seg.k1 - seg.k0) * ds / seg.length};
      if (!rig.walk(at, std::span<const Segment>(&part, 1), step, true, w))
        return s + ds;
      at = w.end;
      s += ds;
    }
  }
  return s; // (the whole walk failed where the steps didn't: at its end)
}

namespace
{
PlanResult planOnce(const PlanRequest& q,
                    const std::function<void(const PlanResult&)>& onPartial);
}

PlanResult planNow(const PlanRequest& q,
                   const std::function<void(const PlanResult&)>& onPartial)
{
  SFS_CPU_SCOPE("nav/plan (workers)");
  double h0, s0, d0, w0, n0;
  takeProfTimes(h0, s0, d0, w0, n0);
  const CheckStats checks0 = checkStats();
  // (Its collision queries: this thread's, before and after.)
  const auto checked = [&](PlanResult& out)
  {
    const CheckStats& now = checkStats();
    out.stats.checks = now.exact - checks0.exact;
    out.stats.broadChecks = now.broad - checks0.broad;
    out.stats.checkMs = now.ms - checks0.ms;
    out.stats.costCalls = now.costs - checks0.costs;
    out.stats.costMs = now.costMs - checks0.costMs;
  };
  PlanResult r = planOnce(q, onPartial);
  takeProfTimes(r.stats.heuristicMs,
                r.stats.shotMs,
                r.stats.headingMs,
                r.stats.wordsMs,
                r.stats.nearMs);
  // (The final check, finer than the search's, found a touch the search's
  // looks missed: searched again looking as finely. Rare.)
  if (r.state == PlanState::Found && r.stats.exactMiss &&
      q.tunables.checkStep > q.tunables.exactStep)
  {
    PlanRequest fine = q;
    fine.tunables.checkStep = q.tunables.exactStep;
    PlanResult again = planOnce(fine, onPartial);
    takeProfTimes(again.stats.heuristicMs,
                  again.stats.shotMs,
                  again.stats.headingMs,
                  again.stats.wordsMs,
                  again.stats.nearMs);
    again.stats.ms += r.stats.ms;
    again.stats.nodes += r.stats.nodes;
    again.stats.heuristicMs += r.stats.heuristicMs;
    again.stats.shotMs += r.stats.shotMs;
    again.stats.headingMs += r.stats.headingMs;
    again.stats.wordsMs += r.stats.wordsMs;
    again.stats.nearMs += r.stats.nearMs;
    checked(again);
    return again;
  }
  checked(r);
  return r;
}

namespace
{
PlanResult planOnce(const PlanRequest& q,
                    const std::function<void(const PlanResult&)>& onPartial)
{
  const auto t0 = std::chrono::steady_clock::now();
  PlanResult r;
  if (!q.space || (q.goals.empty() && !q.goalTest))
  {
    r.state = PlanState::Blocked;
    r.why = "no space or no goal";
    return r;
  }
  const Motions rig(q);
  r.stats.setupMs = msSince(t0);
  // A goal nothing can stand at (something stands on it) is never reached:
  // that's proof at once, not after growing every pose it could reach (a
  // parked truck's corner on a combine's put-away spot: millions of poses
  // across a farm). Not when it starts touching something and the goal is
  // within getting out of that (the way out is let off its touch).
  if (!q.goals.empty() && !q.goalTest)
  {
    const bool startClear = rig.standsClear(q.from, q.hitch);
    std::vector<Space::Hit> onGoals;
    bool reachable = false;
    for (const Goal& g : q.goals)
    {
      if (!startClear &&
          glm::length(g.pose.position - q.from.position) <= kEscapeReach)
      {
        reachable = true;
        break;
      }
      const std::optional<Space::Hit> hit = rig.touches(g.pose, g.hitch);
      if (!hit)
      {
        reachable = true;
        break;
      }
      onGoals.push_back(*hit);
    }
    if (!reachable)
    {
      r.state = PlanState::Blocked;
      r.why = "no way: something stands where it's to stop (at (" +
              std::to_string(onGoals.front().point.x) + ", " +
              std::to_string(onGoals.front().point.y) + "))";
      r.blockers = std::move(onGoals);
      r.stats.ms = msSince(t0);
      return r;
    }
  }
  // The area searched: round the start and goals, widened if the search was
  // hemmed in only by it (no proof), up to everything there is.
  Rect area{q.from.position, q.from.position};
  for (const Goal& g : q.goals)
  {
    area.lo = glm::min(area.lo, g.pose.position);
    area.hi = glm::max(area.hi, g.pose.position);
  }
  Rect world = q.space->bounds();
  world.lo = glm::min(world.lo, area.lo) - 30.0f;
  world.hi = glm::max(world.hi, area.hi) + 30.0f;
  float margin = q.margin;
  PlanRequest local = q;
  // The coarse graph: the tight gaps on the way (shots aimed through them),
  // and for a long trip the corridor the search keeps to.
  std::vector<Pose2> vias;
  if (q.graph && !q.goals.empty())
  {
    const float half =
        std::max(rig.reachY, q.vehicle.trailer ? rig.towY : 0.0f);
    const auto tg = std::chrono::steady_clock::now();
    std::vector<glm::vec2> corridor =
        q.graph->route(q.from.position, q.goals[0].pose.position, half, &vias);
    r.stats.graphMs = msSince(tg);
    if (local.corridor.empty() && glm::length(q.goals[0].pose.position -
                                              q.from.position) > q.corridorFrom)
      local.corridor = std::move(corridor);
  }
  uint32_t budget = std::max<uint32_t>(q.nodeBudget, 1);
  uint32_t spent = 0;
  // A goal hemmed in where it stands (a pocket the rig can't drive out of):
  // proven from its side. The rig's motions are the same driven either way
  // (it may reverse, nothing towed), so a goal is reachable from the start
  // only if the start is from the goal: a search from each goal to the start
  // that grows every pose it can reach within the first budget, never
  // meeting its area's edge, proves there's no way, where the search from
  // the start would grow every pose of the whole map to prove it. Tried
  // once, when the first search hasn't found a way in its first budget.
  bool goalSideTried = static_cast<bool>(q.goalTest) ||
                       q.vehicle.trailer.has_value() || rig.forwardOnly ||
                       !rig.standsClear(q.from, q.hitch);
  const auto goalSideBlocked = [&](Rect a)
  {
    goalSideTried = true;
    uint32_t used = 0;
    for (const Goal& g : q.goals)
    {
      PlanRequest back = q;
      back.from = g.pose;
      back.hitch = 0.0f;
      back.goals = {Goal{q.from}};
      back.corridor.clear();
      const Motions backRig(back);
      const std::unique_ptr<ISearch> search =
          searches().make(q.search, SearchContext{backRig, a, {}, {}});
      if (!search)
        return false;
      const ISearch::Status st =
          search->run(std::max<uint32_t>(q.nodeBudget, 1));
      used += search->grown();
      if (st != ISearch::Status::Exhausted || search->leftArea())
        return false;
    }
    spent += used;
    return true;
  };
  for (;;)
  {
    Rect a{glm::max(area.lo - margin, world.lo),
           glm::min(area.hi + margin, world.hi)};
    const std::unique_ptr<ISearch> search =
        searches().make(q.search, SearchContext{rig, a, local.corridor, vias});
    if (!search)
    {
      r.state = PlanState::Partial;
      r.why = "no search named '" + q.search + "'";
      return r;
    }
    ISearch& s = *search;
    for (;;)
    {
      const ISearch::Status st = s.run(budget > spent ? budget - spent : 0);
      r.stats.nodes = spent + s.grown();
      r.stats.shots = s.shots();
      if (st == ISearch::Status::Found)
      {
        const Way c = s.found();
        s.describe(r.stats);
        r.stats.plain = c.links.size() <= 1;
        r.goal = c.goal;
        const auto tf = std::chrono::steady_clock::now();
        std::vector<Segment> segs;
        const Goal* goal =
            c.goal >= 0 ? &q.goals[static_cast<std::size_t>(c.goal)] : nullptr;
        if (q.smooth)
          segs = smooth(rig, c, goal);
        else
          for (const auto& l : c.links)
            segs.insert(segs.end(), l.begin(), l.end());
        r.stats.smoothMs = msSince(tf);
        finish(rig, q, std::move(segs), r);
        r.stats.finishMs = msSince(tf);
        r.state = PlanState::Found;
        r.stats.ms = msSince(t0);
        return r;
      }
      if (!goalSideTried &&
          (st == ISearch::Status::Budget ||
           (st == ISearch::Status::Exhausted && s.leftArea())) &&
          goalSideBlocked(a))
      {
        r.state = PlanState::Blocked;
        r.stats.nodes = spent + s.grown();
        r.why = "no way: every pose the goal can be reached from searched";
        r.stats.ms = msSince(t0);
        return r;
      }
      if (st == ISearch::Status::Exhausted)
        break;
      if (st == ISearch::Status::Unknown)
      {
        // (A search that can't prove: never "no way", the clear way so far.)
        r.state = PlanState::Partial;
        r.why = "the search gave up without proof";
        r.stats.ms = msSince(t0);
        return r;
      }
      // Out of budget: the clear way so far, and on with twice the budget.
      if (q.cancel && q.cancel->load(std::memory_order_relaxed))
      {
        r.state = PlanState::Partial;
        r.why = "cancelled";
        r.stats.ms = msSince(t0);
        return r;
      }
      ++r.stats.rounds;
      {
        const Way c = s.bestSoFar();
        std::vector<Segment> segs;
        for (const auto& l : c.links)
          segs.insert(segs.end(), l.begin(), l.end());
        PlanResult partial = r;
        finish(rig, q, std::move(segs), partial);
        partial.state = PlanState::Partial;
        partial.why = "out of budget: searching on";
        partial.stats.ms = msSince(t0);
        if (budget >= q.budgetCap)
          return partial;
        if (onPartial)
          onPartial(partial);
      }
      budget = std::min(budget * 2, q.budgetCap);
      // (Kept to the corridor a while without getting there: the corridor's
      // no help (a gap the coarse grid let through that the rig can't
      // thread): on without it.)
      if (!local.corridor.empty() &&
          budget > 4 * std::max<uint32_t>(q.nodeBudget, 1))
        break;
    }
    spent += s.grown();
    if (!local.corridor.empty() &&
        budget > 4 * std::max<uint32_t>(q.nodeBudget, 1))
    {
      ++r.stats.widened;
      local.corridor.clear();
      continue;
    }
    const bool everything = a.lo.x <= world.lo.x && a.lo.y <= world.lo.y &&
                            a.hi.x >= world.hi.x && a.hi.y >= world.hi.y;
    if (s.leftArea() && (!local.corridor.empty() || !everything))
    {
      ++r.stats.widened;
      if (!local.corridor.empty())
        local.corridor.clear();
      else
        margin = margin * 2.5f + 10.0f;
      continue;
    }
    // Proven: every pose it could reach grown, nothing gets there.
    r.state = PlanState::Blocked;
    r.stats.nodes = spent;
    r.why = "no way: every reachable pose searched";
    r.blockers = s.blockers();
    r.stats.ms = msSince(t0);
    return r;
  }
}

} // namespace

PlanScheduler::JobId submitPlan(PlanScheduler& scheduler,
                                uint64_t owner,
                                std::string label,
                                PlanScheduler::Clock::time_point deadline,
                                PlanRequest request,
                                bool lookAhead)
{
  return scheduler.submitOne(
      owner,
      std::move(label),
      deadline,
      [request = std::move(request)](const std::atomic<bool>& cancel) mutable
      {
        request.cancel = &cancel;
        return std::any(planNow(request));
      },
      lookAhead);
}

} // namespace sfs::nav::vehicle