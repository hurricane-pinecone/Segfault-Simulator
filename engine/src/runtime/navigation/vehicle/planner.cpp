#include "engine/runtime/navigation/vehicle/planner.h"

#include "engine/core/jobs/workerPool.h"
#include "engine/runtime/navigation/vehicle/smoother.h"
#include "engine/runtime/navigation/vehicle/speedProfile.h"
#include "engine/runtime/navigation/vehicle/vehicleSearch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace sfs::nav::vehicle
{

void addHybridAStar(Searches& searches); // (hybridAStar.cpp)

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
  if (!rig.walk(q.from, r.path.segments, q.tunables.exactStep, true, w, true))
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
  profile(r.path, q.vehicle, q.speedCap);
  r.seconds = r.path.duration();
}
} // namespace

float priceOf(const PlanRequest& request,
              Pose2 from,
              float hitch,
              const std::vector<Segment>& segments,
              bool check)
{
  const Motions rig(request);
  Motions::Walk w;
  w.hitch = hitch;
  w.escape = check && !rig.standsClear(from, hitch) ? kEscapeReach : 0.0f;
  if (!rig.walk(from, segments, request.tunables.checkStep, check, w))
    return kInf;
  return w.cost;
}

namespace
{
PlanResult planOnce(const PlanRequest& q,
                    const std::function<void(const PlanResult&)>& onPartial);
}

PlanResult planNow(const PlanRequest& q,
                   const std::function<void(const PlanResult&)>& onPartial)
{
  PlanResult r = planOnce(q, onPartial);
  // (The final check, finer than the search's, found a touch the search's
  // looks missed: searched again looking as finely. Rare.)
  if (r.state == PlanState::Found && r.stats.exactMiss &&
      q.tunables.checkStep > q.tunables.exactStep)
  {
    PlanRequest fine = q;
    fine.tunables.checkStep = q.tunables.exactStep;
    PlanResult again = planOnce(fine, onPartial);
    again.stats.ms += r.stats.ms;
    again.stats.nodes += r.stats.nodes;
    return again;
  }
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
      const void* body = rig.probe(false, 0.0f, 0.0f);
      std::optional<Space::Hit> hit = q.space->hit(body, g.pose);
      if (!hit && q.vehicle.trailer && g.hitch)
        hit = q.space->hit(rig.probe(true, 0.0f, 0.0f),
                           trailerPose(*q.vehicle.trailer, g.pose, *g.hitch));
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
    std::vector<glm::vec2> corridor =
        q.graph->route(q.from.position, q.goals[0].pose.position, half, &vias);
    if (local.corridor.empty() && glm::length(q.goals[0].pose.position -
                                              q.from.position) > q.corridorFrom)
      local.corridor = std::move(corridor);
  }
  uint32_t budget = std::max<uint32_t>(q.nodeBudget, 1);
  uint32_t spent = 0;
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
        r.stats.plain = s.grown() == 0;
        const Way c = s.found();
        r.goal = c.goal;
        std::vector<Segment> segs;
        const Goal* goal =
            c.goal >= 0 ? &q.goals[static_cast<std::size_t>(c.goal)] : nullptr;
        if (q.smooth)
          segs = smooth(rig, c, goal);
        else
          for (const auto& l : c.links)
            segs.insert(segs.end(), l.begin(), l.end());
        finish(rig, q, std::move(segs), r);
        r.state = PlanState::Found;
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

struct Planner::Job
{
  PlanRequest request;
  std::atomic<bool> cancel{false};
  std::mutex mutex;
  PlanResult latest;
};
struct Planner::Jobs
{
  std::mutex mutex;
  std::unordered_map<Ticket, std::shared_ptr<Job>> jobs;
};

Planner::Planner(unsigned threads)
    : m_pool(std::make_unique<WorkerPool>(threads)),
      m_jobs(std::make_shared<Jobs>())
{
}

Planner::~Planner()
{
  {
    std::lock_guard lock(m_jobs->mutex);
    for (auto& [t, job] : m_jobs->jobs)
      job->cancel = true;
  }
  m_pool.reset();
}

Planner::Ticket Planner::submit(PlanRequest request)
{
  auto job = std::make_shared<Job>();
  job->request = std::move(request);
  job->request.cancel = &job->cancel;
  const Ticket t = m_next++;
  {
    std::lock_guard lock(m_jobs->mutex);
    m_jobs->jobs[t] = job;
  }
  m_pool->submit(
      [job]
      {
        PlanResult r = planNow(job->request,
                               [&](const PlanResult& partial)
                               {
                                 std::lock_guard lock(job->mutex);
                                 job->latest = partial;
                               });
        std::lock_guard lock(job->mutex);
        job->latest = std::move(r);
      });
  return t;
}

PlanResult Planner::poll(Ticket ticket)
{
  std::shared_ptr<Job> job;
  {
    std::lock_guard lock(m_jobs->mutex);
    const auto it = m_jobs->jobs.find(ticket);
    if (it == m_jobs->jobs.end())
      return {};
    job = it->second;
  }
  std::lock_guard lock(job->mutex);
  return job->latest;
}

void Planner::cancel(Ticket ticket)
{
  std::lock_guard lock(m_jobs->mutex);
  const auto it = m_jobs->jobs.find(ticket);
  if (it == m_jobs->jobs.end())
    return;
  it->second->cancel = true;
  m_jobs->jobs.erase(it);
}

std::size_t Planner::busy() const { return m_pool->pending(); }

} // namespace sfs::nav::vehicle