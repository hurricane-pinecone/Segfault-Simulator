#include "engine/runtime/navigation/fleet.h"

#include "engine/core/util/cpuTiming.h"

#include "engine/core/logger/logger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <string>

namespace sfs::nav
{

namespace
{
constexpr float kForever = std::numeric_limits<float>::infinity();
// s at most one that gave way in a deadlock ranks below those it gave way to.
constexpr float kDeferAtMost = 60.0f;
// s a deadlock nobody could get out of is logged once for (the same ones).
constexpr double kHopelessFor = 120.0;
// Times a way it plans on its own is planned again round a higher one's
// way it would be held standing in (Fleet::planRound()).
constexpr int kAvoidTries = 2;
} // namespace

// One plan on its way: its scheduler job, or (planned in update(), or
// collected) the finished plan.
struct Fleet::Job
{
  PlanScheduler::JobId id = 0;
  std::optional<AgentPlan> plan;
};

struct Fleet::Agent
{
  AgentSpec spec;
  std::shared_ptr<const AgentKind> kind;
  std::unique_ptr<AgentMotion> motion;
  Pose2 pose; // standing with no path
  float hitch = 0.0f;
  int priority = 0;
  State state = State::Idle;

  std::vector<AgentGoal> goals;
  std::function<bool(Pose2)> goalTest;
  uint64_t request = 0;

  Want want = Want::None;  // a plan wanted once it stands
  bool forTraffic = false; // (that plan is for traffic)
  std::shared_ptr<Job> job;
  Want jobFor = Want::None;
  std::shared_ptr<Job> aheadJob; // its look-ahead plan (planAhead())
  float capScale = 1.0f; // (a plan out of its cap: the next one's bigger)

  uint64_t pathId = 0;
  bool done = true;        // at its path's end (or none)
  bool onTrip = false;     // its path ends at its goal
  bool toWaitSpot = false; // driving to a wait spot (or planning it)
  bool stuck = false;      // in a higher one's way, no wait spot: solid
  bool blockedAtEnd = false;
  bool arrived = false, blocked = false;
  AgentId waitFrom = 0;   // whose way it went to a wait spot off
  AgentId waitingFor = 0; // whose reservation or zone it stands for
  Traffic::Schedule::Why why = Traffic::Schedule::Why::None; // (last)

  float speedCap = std::numeric_limits<float>::infinity(); // (the game's)
  float standingFor = 0.0f;
  float stopAt = std::numeric_limits<float>::infinity(); // (last told)
  bool replanning = false;   // (stood for the game to plan it again)
  bool gamePlanning = false; // (its game planning its next path: setPlanning())
  float waitedFor = 0.0f;    // s stood held by another (Traffic::waitsOn())
  // Yielding to break a deadlock: the others whose claimed ground its wait
  // spot is clear of.
  std::vector<AgentId> yieldTo;
  bool yieldFailed = false; // (no wait spot, or held on its way: next one)
  // Gave way in a deadlock: ranked below these till it's on its way free of
  // them (they go through first, never stood for it again at once).
  std::vector<AgentId> deferTo;
  float deferredFor = 0.0f;
  std::vector<Space::Hit> blockers;
  Counters counters;
  // Its way planned again round these: the ground of those ranked above it
  // that its last plan would have held it standing in (planRound()).
  std::vector<Footprint> avoid;
  int avoided = 0;
  std::optional<AgentPlan> firstTry; // (a trip's, if planned round fails)
  void avoidNone()
  {
    avoid.clear();
    avoided = 0;
    firstTry.reset();
  }
  // The way last handed back for running into a static obstacle (whose, and
  // where that way went): the same way back from its game for the same one
  // isn't handed back again (planned again, it'd come back the same).
  AgentId solidBy = 0;
  Pose2 solidEnd;
  float solidLength = -1.0f;
  bool solidHeld = false; // (standing short of it for that: logged once)

  // Its narrow stretches (m along its path), for the path they're of.
  uint64_t narrowsOf = 0;
  std::vector<std::pair<float, float>> narrows;

  // Driving a path the game made (setPath): no goal of its own to plan.
  bool gameLed() const { return goals.empty() && !goalTest; }
  bool hasPath() const
  {
    return motion->path() && motion->path()->samples.size() >= 2;
  }
  // (Driven by hand: its motion has it, no path.)
  bool moved() const { return hasPath() || motion->handDriven(); }
  Pose2 where() const { return moved() ? motion->pose() : pose; }
  float hitchNow() const { return moved() ? motion->hitch() : hitch; }
  float speed() const { return moved() ? motion->speed() : 0.0f; }
  void follow(std::shared_ptr<const AgentPath> path, float fromS = 0.0f)
  {
    const float v = std::abs(speed());
    pose = where();
    hitch = hitchNow();
    motion->setPath(std::move(path), v, fromS);
    ++pathId;
    stopAt = std::numeric_limits<float>::infinity();
    done = !hasPath();
  }
  void stand()
  {
    pose = where();
    hitch = hitchNow();
    motion->setPath(nullptr, 0.0f);
    ++pathId;
    stopAt = std::numeric_limits<float>::infinity();
    done = true;
  }
};

Fleet::Fleet() : Fleet(Settings{}) {}

Fleet::Fleet(Settings settings, std::shared_ptr<PlanScheduler> scheduler)
    : m_settings(settings), m_scheduler(std::move(scheduler))
{
}

Fleet::~Fleet()
{
  if (m_scheduler)
    for (auto& [id, a] : m_agents)
    {
      if (a->job && a->job->id)
        m_scheduler->cancel(a->job->id);
      if (a->aheadJob && a->aheadJob->id)
        m_scheduler->cancel(a->aheadJob->id);
    }
}

void Fleet::setWorld(World world)
{
  m_world = std::move(world);
  if (m_world.cost)
  {
    std::shared_ptr<const CostLayer> cost = m_world.cost;
    m_traffic.setNoWait([cost](glm::vec2 p) { return cost->noWait(p); });
  }
  else
    m_traffic.setNoWait({});
}

bool Fleet::add(AgentId id, AgentSpec spec, Pose2 pose, float hitch)
{
  std::shared_ptr<const AgentKind> kind = AgentKinds::find(spec.kind);
  if (!kind)
    return false;
  auto a = std::make_unique<Agent>();
  a->motion = kind->makeFollower(spec);
  a->spec = std::move(spec);
  a->kind = std::move(kind);
  a->pose = pose;
  a->hitch = hitch;
  remove(id);
  m_agents.emplace(id, std::move(a));
  return true;
}

void Fleet::remove(AgentId id)
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end())
    return;
  if (m_scheduler)
    for (const std::shared_ptr<Job>& job :
         {it->second->job, it->second->aheadJob})
      if (job && job->id)
        m_scheduler->cancel(job->id);
  m_agents.erase(it);
  m_traffic.remove(id);
}

void Fleet::setSpec(AgentId id, AgentSpec spec)
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end())
    return;
  Agent& a = *it->second;
  if (spec.kind != a.spec.kind)
  {
    std::shared_ptr<const AgentKind> kind = AgentKinds::find(spec.kind);
    if (!kind)
      return;
    a.stand();
    a.kind = std::move(kind);
    a.motion = a.kind->makeFollower(spec);
  }
  else if (a.motion)
    a.motion->setSpec(spec);
  a.spec = std::move(spec);
}

const AgentSpec* Fleet::spec(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it == m_agents.end() ? nullptr : &it->second->spec;
}

void Fleet::setPriority(AgentId id, int priority)
{
  if (const auto it = m_agents.find(id); it != m_agents.end())
    it->second->priority = priority;
}

void Fleet::setHitch(AgentId id, float hitch)
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end() || it->second->moved())
    return;
  it->second->hitch = hitch;
}

void Fleet::place(AgentId id, Pose2 pose, float hitch)
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end())
    return;
  Agent& a = *it->second;
  cancel(id);
  if (a.motion->handDriven())
  {
    // (Put somewhere while driven by hand: driven on from there.)
    a.motion->takeWheel(pose, hitch, 0.0f);
    a.pose = pose;
    a.hitch = hitch;
    return;
  }
  a.motion->setPath(nullptr, 0.0f);
  ++a.pathId;
  a.pose = pose;
  a.hitch = hitch;
}

uint64_t Fleet::requestGoal(AgentId id, AgentGoal goal)
{
  return requestGoals(id, {goal});
}

uint64_t Fleet::requestGoals(AgentId id,
                             std::vector<AgentGoal> goals,
                             std::function<bool(Pose2)> goalTest)
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end())
    return 0;
  handBack(id); // (sent somewhere: the driver's let go)
  Agent& a = *it->second;
  a.goals = std::move(goals);
  a.goalTest = std::move(goalTest);
  a.request = m_nextRequest++;
  a.replanning = false;
  a.arrived = a.blocked = a.blockedAtEnd = false;
  a.onTrip = a.toWaitSpot = a.stuck = false;
  a.yieldTo.clear();
  a.blockers.clear();
  a.avoidNone();
  a.capScale = 1.0f;
  ask(id, a, Want::Trip);
  return a.request;
}

void Fleet::setPath(AgentId id, std::shared_ptr<const AgentPath> path)
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end())
    return;
  handBack(id); // (given a path: the driver's let go)
  Agent& a = *it->second;
  cancel(id);
  a.request = m_nextRequest++;
  a.follow(std::move(path));
  a.onTrip = true;
  a.replanning = false;
  a.stuck = false;
}

void Fleet::setSpeedCap(AgentId id, float cap)
{
  if (const auto it = m_agents.find(id); it != m_agents.end())
    it->second->speedCap = std::max(cap, 0.0f);
}

void Fleet::setPlanning(AgentId id, bool planning)
{
  if (const auto it = m_agents.find(id); it != m_agents.end())
    it->second->gamePlanning = planning;
}

void Fleet::cancel(AgentId id)
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end())
    return;
  Agent& a = *it->second;
  if (a.job && a.job->id && m_scheduler)
    m_scheduler->cancel(a.job->id);
  a.job = nullptr;
  a.want = Want::None;
  // (Kept: a game that cancels while it plans the one handed back (NeedsPlan)
  // still has work for it; its next path or goal ends that.)
  a.goals.clear();
  a.goalTest = {};
  a.onTrip = a.toWaitSpot = a.stuck = false;
  a.yieldTo.clear();
  a.blocked = a.blockedAtEnd = a.arrived = false;
  if (a.hasPath() && !a.done)
    a.stand();
}

bool Fleet::takeWheel(AgentId id)
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end())
    return false;
  Agent& a = *it->second;
  if (a.motion->handDriven())
    return true;
  const Pose2 at = a.where();
  const float hitch = a.hitchNow(), speed = a.speed();
  cancel(id);
  a.replanning = false;
  a.waitingFor = 0;
  a.pose = at;
  a.hitch = hitch;
  ++a.pathId;
  a.done = true;
  return a.motion->takeWheel(at, hitch, speed);
}

void Fleet::setControls(AgentId id, const Controls& controls)
{
  if (const auto it = m_agents.find(id); it != m_agents.end())
    it->second->motion->setControls(controls);
}

void Fleet::handBack(AgentId id)
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end() || !it->second->motion->handDriven())
    return;
  Agent& a = *it->second;
  a.pose = a.where();
  a.hitch = a.hitchNow();
  a.motion->handBack();
  a.motion->setPath(nullptr, 0.0f);
  ++a.pathId;
  a.done = true;
  a.standingFor = 0.0f;
}

bool Fleet::handDriven(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it != m_agents.end() && it->second->motion->handDriven();
}

void Fleet::cooperate(AgentId a, AgentId b, bool on)
{
  m_traffic.cooperate(a, b, on);
}

World Fleet::planWorld(AgentId except, const std::vector<Footprint>& more) const
{
  std::vector<BoxSpace::Solid> solids;
  const auto put = [&](const Footprint& f, uint64_t id) {
    solids.push_back({f.centre, f.half, f.heading, 0.0f, 6.0f, false, id});
  };
  for (const auto& [id, a] : m_agents)
    if (id != except && solid(*a))
      for (const Footprint& f :
           a->kind->footprint(a->spec, a->where(), a->hitchNow()))
        put(f, id);
  for (const Footprint& f : more)
    put(f, 0);
  World w = m_world;
  if (!solids.empty() && w.space)
    w.space = std::make_shared<SpaceWithSolids>(w.space, std::move(solids));
  return w;
}

bool Fleet::standing(const Agent& a) const { return !a.hasPath() || a.done; }

// Stood still a while with no plan on the way, not for traffic or held by
// its game (a speed cap of 0, whatever traffic says). One standing for
// traffic moves on once the one above has passed: never a solid for that.
bool Fleet::parked(const Agent& a) const
{
  return a.standingFor >= m_settings.parkedAfter && a.want == Want::None &&
         !a.job && !a.gamePlanning &&
         (a.speedCap <= 0.0f ||
          (a.why == Traffic::Schedule::Why::None && !a.toWaitSpot));
}

// Nothing to do: no path to drive (or at its end), no plan on the way (its
// own or its game's), not stood for the game to plan it again.
bool Fleet::still(const Agent& a) const
{
  return standing(a) && a.want == Want::None && !a.job && !a.replanning &&
         !a.toWaitSpot && !a.gamePlanning;
}

bool Fleet::solid(const Agent& a) const
{
  return still(a) || parked(a) || a.stuck;
}

bool Fleet::solid(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it != m_agents.end() && solid(*it->second);
}

AgentPlan Fleet::planNow(AgentId id, PlanAsk ask) const
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end())
    return {};
  const Agent& a = *it->second;
  if (!ask.world.space)
    ask.world = planWorld(id, {});
  return a.kind->plan(a.spec, ask, {});
}

bool Fleet::planAhead(AgentId id, PlanAsk ask)
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end())
    return false;
  Agent& a = *it->second;
  if (a.aheadJob && a.aheadJob->id && m_scheduler)
    m_scheduler->cancel(a.aheadJob->id);
  a.aheadJob = nullptr;
  // From where its path ends, needed by when it gets there (real seconds:
  // its speed profile's time left, at the game's pace).
  double left = 0.0;
  if (a.hasPath() && !a.done)
  {
    const AgentPath& path = *a.motion->path();
    const AgentPath::Sample& end = path.samples.back();
    ask.from = end.pose;
    ask.hitch = end.hitch;
    const std::size_t i = std::min(a.motion->index(), path.samples.size() - 1);
    left = std::max(0.0f, end.time - path.samples[i].time) /
           std::max(m_simPerReal, 1e-3);
  }
  else
  {
    ask.from = a.where();
    ask.hitch = a.hitchNow();
  }
  if (!ask.world.space)
    ask.world = planWorld(id, {});
  const auto deadline =
      PlanScheduler::Clock::now() +
      std::chrono::duration_cast<PlanScheduler::Clock::duration>(
          std::chrono::duration<double>(left));
  a.aheadJob = start(id, a, std::move(ask), "next way", deadline, true);
  return true;
}

std::optional<AgentPlan> Fleet::ahead(AgentId id)
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end() || !it->second->aheadJob)
    return std::nullopt;
  Agent& a = *it->second;
  std::optional<AgentPlan> plan = finished(m_scheduler.get(), *a.aheadJob);
  if (plan)
    a.aheadJob = nullptr;
  return plan;
}

bool Fleet::planningAhead(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it != m_agents.end() && it->second->aheadJob != nullptr;
}

std::shared_ptr<Fleet::Job>
Fleet::start(AgentId id,
             const Agent& a,
             PlanAsk ask,
             const char* what,
             PlanScheduler::Clock::time_point deadline,
             bool lookAhead)
{
  auto job = std::make_shared<Job>();
  if (!m_scheduler)
  {
    job->plan = a.kind->plan(a.spec, ask, {});
    return job;
  }
  // (Only the finished plan: the clear way so far is never driven.)
  job->id = m_scheduler->submitOne(
      id,
      "agent " + std::to_string(id) + ": " + what,
      deadline,
      [kind = a.kind, spec = a.spec, ask = std::move(ask)](
          const std::atomic<bool>& cancel) mutable
      {
        ask.cancel = &cancel;
        return std::any(kind->plan(spec, ask, {}));
      },
      lookAhead);
  return job;
}

std::optional<AgentPlan> Fleet::finished(PlanScheduler* scheduler, Job& job)
{
  if (job.plan || !scheduler)
    return job.plan;
  PlanScheduler::Collected c = scheduler->collect(job.id);
  for (const std::any& leg : c.legs)
    if (const AgentPlan* p = std::any_cast<AgentPlan>(&leg))
      job.plan = *p;
  if (!job.plan && (c.state == PlanScheduler::State::Failed ||
                    c.state == PlanScheduler::State::Unknown ||
                    c.state == PlanScheduler::State::Done))
  {
    // (Failed, logged: nothing clear, as a search that found nothing.)
    AgentPlan none;
    none.state = PlanState::Partial;
    none.why = "its plan failed";
    job.plan = std::move(none);
  }
  return job.plan;
}

void Fleet::ask(AgentId, Agent& a, Want want)
{
  if (a.job && a.job->id && m_scheduler)
    m_scheduler->cancel(a.job->id);
  a.job = nullptr;
  a.want = want;
}

void Fleet::submit(AgentId id, Agent& a)
{
  PlanAsk ask;
  ask.from = a.where();
  ask.hitch = a.hitchNow();
  if (a.want == Want::WaitSpot)
  {
    // Anywhere its body is clear of every reservation ranked above it (and
    // may wait); the body of the one it's in the way of and the narrow
    // ground others hold are solids on the way there.
    // (Not ground it's in already: it goes out through that.)
    const std::vector<Footprint> here =
        a.kind->footprint(a.spec, a.where(), a.hitchNow());
    // (Each held stretch it's in left out whole, not just the part under
    // it: backing out along a lane another holds, all of it behind it was
    // a solid.)
    std::vector<Footprint> solids = m_traffic.heldBesides(id, here);
    // (Breaking a deadlock: every other member's body a solid on the way, and
    // all the ground each claims to be clear of, whatever their ranks.)
    std::vector<AgentId> others = a.yieldTo;
    if (std::find(others.begin(), others.end(), a.waitFrom) == others.end())
      others.push_back(a.waitFrom);
    // (Those it yields to in a deadlock kept the traffic margin off, not
    // just the planners' gap: going by them it's never in their way, so
    // none of them goes off to a wait spot too.)
    for (const AgentId oid : others)
      if (const auto o = m_agents.find(oid); o != m_agents.end())
      {
        const bool yieldingTo =
            std::find(a.yieldTo.begin(), a.yieldTo.end(), oid) !=
            a.yieldTo.end();
        for (Footprint f : o->second->kind->footprint(
                 o->second->spec, o->second->where(), o->second->hitchNow()))
        {
          if (yieldingTo)
            f.half += glm::vec2(m_traffic.margin());
          solids.push_back(f);
        }
      }
    solids.insert(solids.end(), a.avoid.begin(), a.avoid.end());
    ask.world = planWorld(id, solids);
    std::vector<Footprint> reserved = m_traffic.reservedAbove(id);
    for (const AgentId oid : a.yieldTo)
      for (const Footprint& f : m_traffic.claimed(oid))
        reserved.push_back(f);
    // (A little more room than Traffic asks: it waits clear, not on the
    // edge of the other's way.)
    const float margin = m_traffic.margin() + 0.25f;
    std::shared_ptr<const AgentKind> kind = a.kind;
    std::shared_ptr<const CostLayer> cost = m_world.cost;
    ask.goalTest = [reserved, margin, kind, spec = a.spec, cost](Pose2 p)
    {
      if (cost && cost->noWait(p.position))
        return false;
      return !hits(kind->footprint(spec, p, 0.0f), reserved, margin);
    };
    ask.nodeBudget = m_settings.waitSpotBudget;
    ask.budgetCap = m_settings.waitSpotCap;
  }
  else
  {
    ask.world = planWorld(id, a.avoid);
    ask.goals = a.goals;
    ask.goalTest = a.goalTest;
    ask.mode = a.goals.empty() ? 0 : a.goals.front().mode;
    ask.nodeBudget = m_settings.nodeBudget;
    ask.budgetCap = static_cast<uint32_t>(
        std::min(4.0e9, double(m_settings.budgetCap) * a.capScale));
  }
  a.jobFor = a.want;
  a.want = Want::None;
  ++a.counters.plans;
  if (a.forTraffic)
    ++a.counters.replans;
  a.forTraffic = false;
  // (Asked standing: needed now.)
  a.job = start(id,
                a,
                std::move(ask),
                a.jobFor == Want::WaitSpot ? "wait spot" : "trip",
                PlanScheduler::Clock::now(),
                false);
}

void Fleet::poll(AgentId id, Agent& a)
{
  if (!a.job)
    return;
  std::optional<AgentPlan> plan = finished(m_scheduler.get(), *a.job);
  if (!plan)
    return;
  a.job = nullptr;
  take(id, a, *plan);
}

Fleet::Round Fleet::planRound(AgentId id, Agent& a, const AgentPlan& plan)
{
  // Its way as it will claim it, all of it, times from now (it stands).
  Traffic::Claim way;
  way.priority = a.priority;
  way.body = a.kind->footprint(a.spec, a.where(), a.hitchNow());
  way.here = way.body.size();
  way.still = true;
  way.leaving = a.toWaitSpot ? a.waitFrom : 0;
  way.leavingAll = a.toWaitSpot && !a.yieldTo.empty();
  const std::vector<AgentPath::Sample>& p = plan.path->samples;
  long last = -1;
  for (std::size_t k = 0; k < p.size(); ++k)
  {
    const long cell = static_cast<long>(p[k].s / m_settings.claimEvery);
    if (cell == last && k + 1 != p.size())
      continue;
    last = cell;
    way.ahead.push_back({p[k].s,
                         p[k].time - 0.5f,
                         p[k].time + 0.5f,
                         a.kind->footprint(a.spec, p[k].pose, p[k].hitch)});
  }
  // (A trip may wait anywhere clear short of a meeting; one going to a wait
  // spot only where it stands now: it's off to wait clear of them.)
  const Traffic::Crossing x =
      m_traffic.crossing(id, way, a.jobFor == Want::Trip);
  if (x.at < 0)
    return Round::Clear;
  const Pose2 at = p[std::min<std::size_t>(
                         std::lower_bound(p.begin(),
                                          p.end(),
                                          way.ahead[x.at].s,
                                          [](const AgentPath::Sample& q,
                                             float s) { return q.s < s; }) -
                             p.begin(),
                         p.size() - 1)]
                       .pose;
  char where[96];
  std::snprintf(
      where, sizeof(where), " at (%.1f, %.1f)", at.position.x, at.position.y);
  const char* what = a.jobFor == Want::WaitSpot ? "wait spot" : "trip";
  if (a.avoided >= kAvoidTries)
  {
    LOG_INFO("Navigation: " + nameOf(id) + "'s way to its " + what +
             " would still hold it in " + nameOf(x.by) + "'s way" + where);
    return Round::Held;
  }
  ++a.avoided;
  // (That one's way a solid on the next one, the traffic margin kept off
  // it, but for the ground it's in now: it goes out through that.)
  for (Footprint f : m_traffic.claimed(x.by))
  {
    f.half += glm::vec2(m_traffic.margin());
    if (!hits({f}, way.body))
      a.avoid.push_back(f);
  }
  if (!a.firstTry)
    a.firstTry = plan;
  LOG_INFO("Navigation: " + nameOf(id) + "'s way to its " + what +
           " would hold it standing in " + nameOf(x.by) + "'s way" + where +
           ": planned again round that one's way");
  ++a.counters.replans;
  ask(id, a, a.jobFor);
  return Round::Again;
}

void Fleet::take(AgentId id, Agent& a, const AgentPlan& plan)
{
  const bool usable = plan.path && plan.path->length() > 0.05f;
  const bool found =
      usable && (a.jobFor == Want::WaitSpot || plan.state == PlanState::Found);
  // (No way round that one's way: the first way after all. It waits where
  // it's held (clear of them), or it's in the way: a wait spot.)
  const auto takeFirst = [&]
  {
    const AgentPlan first = std::move(*a.firstTry);
    a.firstTry.reset();
    a.avoided = kAvoidTries + 1; // (checked: not again)
    take(id, a, first);
  };
  if (a.firstTry && !found)
  {
    takeFirst();
    return;
  }
  if (found && a.avoided <= kAvoidTries)
    switch (planRound(id, a, plan))
    {
    case Round::Clear:
      break;
    case Round::Again:
      return;
    case Round::Held:
      if (a.firstTry)
      {
        takeFirst();
        return;
      }
      break;
    }
  if (a.jobFor == Want::WaitSpot)
  {
    // (No wait spot, or it's as clear as it gets where it stands: it can't
    // get out of the way; a static obstacle the others plan round.)
    if (!usable)
    {
      // (It stands where it is: if the other then stands for it, a deadlock
      // the next one in it breaks; nobody able to, it's stuck then.)
      a.toWaitSpot = false;
      if (!a.yieldTo.empty())
        LOG_INFO("Navigation: " + nameOf(id) +
                 " found no wait spot clear of the others' ways (" + plan.why +
                 "): the next one yields");
      a.yieldTo.clear();
      a.deferTo.clear();
      a.yieldFailed = true;
      return;
    }
    if (!a.yieldTo.empty())
    {
      const Pose2 to = plan.path->samples.back().pose;
      char text[160];
      std::snprintf(text,
                    sizeof(text),
                    " to a wait spot at (%.1f, %.1f), %.1f m",
                    to.position.x,
                    to.position.y,
                    plan.path->length());
      LOG_INFO("Navigation: " + nameOf(id) + text +
               ", clear of the others' ways (breaking a deadlock)");
    }
    a.follow(plan.path);
    a.toWaitSpot = true;
    a.avoidNone();
    return;
  }
  if (plan.state == PlanState::Blocked)
  {
    a.blockers = plan.blockers;
    a.onTrip = false;
    if (standing(a))
    {
      a.blocked = true;
      event(Event::Kind::Blocked, id, a.request, 0, a.blockers);
    }
    else
      a.blockedAtEnd = true;
    return;
  }
  if (plan.state == PlanState::Found && !usable)
  {
    // (There already.)
    a.arrived = true;
    event(Event::Kind::Arrived, id, a.request);
    return;
  }
  if (plan.state != PlanState::Found)
  {
    // Out of its cap short of its goal: the clear way so far is never
    // driven (a partial leg); searched again with a bigger cap, standing.
    if (a.capScale < 8.0f)
    {
      a.capScale *= 2.0f;
      ask(id, a, Want::Trip);
      return;
    }
    LOG_WARN("Navigation: agent " + std::to_string(id) +
             " found no way to its goal within its search budget (" + plan.why +
             "): standing");
    return;
  }
  a.follow(plan.path);
  a.onTrip = true;
  a.avoidNone();
}

// Its narrow stretches from `s` on (to wayReach), each the ground its body
// covers along it. Narrow: one as wide as it can't pass it on either side
// (the statics hem both sides in: a gateway, a lane between buildings).
// Stretches with less than two of its lengths between them are one: it
// takes them all at once, never holding one while it waits for the next.
std::vector<Traffic::Zone> Fleet::zones(Agent& a, float s, std::size_t from)
{
  std::vector<Traffic::Zone> out;
  const AgentPath* path = a.motion->path();
  if (!path || !m_world.space || a.spec.boxes.empty())
    return out;
  const std::vector<AgentPath::Sample>& p = path->samples;
  if (a.narrowsOf != a.pathId)
  {
    a.narrowsOf = a.pathId;
    a.narrows.clear();
    Box3 all = a.spec.boxes.front();
    float half = 0.0f;
    for (const Box3& b : a.spec.boxes)
    {
      all.back = std::min(all.back, b.back);
      all.front = std::max(all.front, b.front);
      all.bottom = std::min(all.bottom, b.bottom);
      all.top = std::max(all.top, b.top);
      half = std::max(half, std::abs(b.shift) + b.half);
    }
    const float gap = m_traffic.margin();
    std::array<std::shared_ptr<const void>, 2> probe;
    for (int side = 0; side < 2; ++side)
    {
      Box3 b = all;
      b.half = half; // (another as wide)
      b.shift = (side ? 1.0f : -1.0f) * (2.0f * half + gap);
      probe[side] =
          m_world.space->prepare(std::span<const Box3>(&b, 1), 0.0f, 0.0f);
    }
    const float length = all.front - all.back;
    float last = -1.0e9f;
    for (const AgentPath::Sample& q : p)
    {
      if (q.s - last < m_settings.claimEvery && &q != &p.back())
        continue;
      last = q.s;
      if (m_world.space->fits(probe[0].get(), q.pose) ||
          m_world.space->fits(probe[1].get(), q.pose))
        continue;
      if (!a.narrows.empty() && q.s - a.narrows.back().second < 2.0f * length)
        a.narrows.back().second = q.s;
      else
        a.narrows.push_back({q.s, q.s});
    }
  }
  for (const auto& [lo, hi] : a.narrows)
  {
    if (hi < s)
      continue;
    if (lo > s + m_settings.wayReach)
      break;
    Traffic::Zone z{lo, hi, {}};
    float last = -1.0e9f;
    for (std::size_t k = from; k < p.size() && p[k].s <= hi; ++k)
      if (p[k].s >= lo && (p[k].s - last >= m_settings.claimEvery ||
                           k + 1 == p.size() || p[k + 1].s > hi))
      {
        last = p[k].s;
        for (const Footprint& f :
             a.kind->footprint(a.spec, p[k].pose, p[k].hitch))
          z.area.push_back(f);
      }
    if (!z.area.empty())
      out.push_back(std::move(z));
  }
  return out;
}

void Fleet::claim(AgentId id, Agent& a, float dt)
{
  Traffic::Claim c;
  c.priority = a.priority;
  // Giving way in a deadlock: above those it gives way to while it goes to
  // its wait spot (they let it in), below them once there, till it's on its
  // way free of them (they go through first).
  for (const AgentId m : a.deferTo)
    if (const auto o = m_agents.find(m); o != m_agents.end())
      c.priority = a.toWaitSpot && !a.yieldTo.empty()
                       ? std::max(c.priority, o->second->priority + 1)
                       : std::min(c.priority, o->second->priority - 1);
  c.body = a.kind->footprint(a.spec, a.where(), a.hitchNow());
  c.here = c.body.size();
  c.standing = standing(a);
  c.solid = solid(a);
  c.still = std::abs(a.speed()) < kStill;
  c.leaving = a.toWaitSpot ? a.waitFrom : 0;
  c.leavingAll = a.toWaitSpot && !a.yieldTo.empty();
  if (!c.standing)
  {
    c.s = a.motion->s();
    const std::vector<AgentPath::Sample>& p = a.motion->path()->samples;
    const std::size_t i0 = std::min(a.motion->index(), p.size() - 1);
    const float t0 = p[i0].time;
    // (Samples on a fixed grid along the path, the same whichever frame
    // it's claimed in; its end too: where it will stand.)
    long last = -1;
    const auto onGrid = [&](std::size_t k)
    {
      const long cell = static_cast<long>(p[k].s / m_settings.claimEvery);
      if (cell == last && k + 1 != p.size())
        return false;
      last = cell;
      return true;
    };
    const auto boxesAt = [&](std::size_t k)
    { return a.kind->footprint(a.spec, p[k].pose, p[k].hitch); };
    // Its stopping stretch is its body: braking now, it still goes there
    // (never past where it was told to stand: it stands there). Every
    // claimEvery along it, and where it stands at its end.
    const float stopTo =
        std::min(c.s + a.motion->stoppingDistance(dt), a.stopAt);
    for (std::size_t k = i0 + 1; k < p.size() && p[k].s <= stopTo; ++k)
      if (onGrid(k) || k + 1 == p.size() || p[k + 1].s > stopTo)
        for (const Footprint& f : boxesAt(k))
          c.body.push_back(f);
    last = -1;
    std::size_t k = i0;
    for (; k < p.size() && p[k].time - t0 < m_settings.claimSeconds; ++k)
      if (onGrid(k))
        c.ahead.push_back(
            {p[k].s, p[k].time - t0 - 0.5f, p[k].time - t0 + 0.5f, boxesAt(k)});
    for (; k < p.size() && p[k].s - c.s <= m_settings.wayReach; ++k)
      if (onGrid(k))
        for (const Footprint& f : boxesAt(k))
          c.way.push_back(f);
    c.zones = zones(a, c.s, i0);
  }
  m_traffic.update(id, std::move(c));
}

void Fleet::drive(AgentId id, Agent& a, float dt)
{
  using Why = Traffic::Schedule::Why;
  const auto finish = [&]
  {
    a.state = a.blocked                                        ? State::Blocked
              : a.arrived                                      ? State::Arrived
              : a.toWaitSpot                                   ? State::Yielding
              : (a.want != Want::None || a.job) && standing(a) ? State::Planning
              : a.waitingFor && !standing(a)                   ? State::Yielding
              : !standing(a)                                   ? State::Moving
                                                               : State::Idle;
  };
  a.standingFor = std::abs(a.speed()) < kStill ? a.standingFor + dt : 0.0f;
  if (a.motion->handDriven())
  {
    // (A driver at the wheel: its controls move it, nothing else does.)
    a.motion->step(dt, a.speedCap, kForever);
    a.state = std::abs(a.speed()) >= kStill ? State::Moving : State::Idle;
    return;
  }
  const Traffic::Schedule sched = m_traffic.scheduleOf(id);
  a.why = standing(a) ? Why::None : sched.why;

  if (a.want != Want::None)
  {
    // Stand first, then plan from there.
    if (!standing(a) && std::abs(a.speed()) >= kStill)
    {
      a.stopAt = kForever;
      a.motion->step(dt, 0.0f, kForever);
    }
    else if (m_world.space) // (no world yet: it waits for one)
      submit(id, a);
    finish();
    return;
  }
  if (standing(a))
  {
    finish();
    return;
  }
  // (Standing for a higher reservation or a zone: it waits for that one.)
  const AgentId waitingFor =
      sched.why == Why::Reserved || sched.why == Why::Zone ? sched.by : 0;
  if (waitingFor && waitingFor != a.waitingFor)
    event(Event::Kind::Yielding, id, a.request, waitingFor);
  else if (!waitingFor && a.waitingFor)
    event(Event::Kind::Resumed, id, a.request, a.waitingFor);
  a.waitingFor = waitingFor;
  if (std::abs(a.speed()) >= kStill)
  {
    a.solidBy = 0;   // (on its way again: a new meeting's handed back afresh)
    a.stuck = false; // (moving: out of the way after all)
    if (!a.toWaitSpot)
      a.yieldFailed = false;
  }
  // Gave way in a deadlock: back to its own rank once it's on its way, free
  // of those it gave way to (they've gone through).
  if (!a.deferTo.empty())
  {
    a.deferredFor += dt;
    const bool heldByThem =
        sched.why != Why::None &&
        std::find(a.deferTo.begin(), a.deferTo.end(), sched.by) !=
            a.deferTo.end();
    if ((!a.toWaitSpot && std::abs(a.speed()) >= kStill && !heldByThem) ||
        a.deferredFor > kDeferAtMost)
      a.deferTo.clear();
  }
  if (sched.why == Why::Solid)
  {
    // A static obstacle on its way (it stopped there after this path was
    // planned, or is stuck in the way): planned round at once, braking to a
    // stand short of it meanwhile.
    if (a.gameLed())
    {
      // (Braked to a stand at its own rate first, then the game's plan.)
      if (std::abs(a.speed()) >= kStill)
      {
        a.stopAt = sched.stopAt;
        a.motion->step(dt, 0.0f, sched.stopAt);
        finish();
        return;
      }
      // (The way it was handed back for already, back the same from its
      // game for the same one: no use planning it again. It stands short of
      // that one, waiting for it to move; logged once.)
      const AgentPath* p = a.motion->path();
      const Pose2 end = p ? p->samples.back().pose : a.where();
      const float length = p ? p->length() : 0.0f;
      if (p && a.solidBy == sched.by &&
          glm::distance(end.position, a.solidEnd.position) < 0.1f &&
          std::abs(wrapAngle(end.heading - a.solidEnd.heading)) < 0.05f &&
          std::abs(length - a.solidLength) < 0.25f)
      {
        if (!a.solidHeld)
        {
          a.solidHeld = true;
          LOG_INFO("Navigation: " + nameOf(id) +
                   "'s way planned again still "
                   "runs into " +
                   nameOf(sched.by) +
                   " (a static obstacle): standing short of it till it moves");
        }
        a.stopAt = sched.stopAt;
        a.motion->step(dt, 0.0f, sched.stopAt);
        finish();
        return;
      }
      a.solidBy = sched.by;
      a.solidEnd = end;
      a.solidLength = length;
      a.solidHeld = false;
      ++a.counters.roundSolid;
      a.stand();
      a.replanning = true;
      event(Event::Kind::NeedsPlan, id, a.request, sched.by);
      finish();
      return;
    }
    if (std::abs(a.speed()) >= kStill)
    {
      a.stopAt = sched.stopAt;
      a.motion->step(dt, 0.0f, sched.stopAt); // (stands, then plans)
      finish();
      return;
    }
    ++a.counters.roundSolid;
    a.forTraffic = true;
    a.stand();
    ask(id, a, Want::Trip);
    finish();
    return;
  }
  if (sched.inTheWay && !a.toWaitSpot && !a.stuck && !a.yieldFailed &&
      a.speedCap > 0.0f)
  {
    // In a higher one's way with nowhere clear on its path: to a wait spot.
    a.waitFrom = sched.by;
    ++a.counters.waitSpots;
    if (a.waitingFor != sched.by)
      event(Event::Kind::Yielding, id, a.request, sched.by);
    a.waitingFor = sched.by;
    a.forTraffic = true;
    a.toWaitSpot = true;
    a.avoidNone();
    ask(id, a, Want::WaitSpot);
    a.stopAt = sched.stopAt;
    a.motion->step(dt, 0.0f, sched.stopAt); // (stands, then plans)
    finish();
    return;
  }
  float cap = kForever;
  for (const AgentGoal& g : a.goals)
    cap = std::min(cap, g.speedCap);
  cap = std::min(cap, a.speedCap);
  // (Easing off to reach its stop as the crossing there clears.)
  cap = std::min(cap, sched.easeTo);
  a.stopAt = sched.stopAt;
  if (sched.why != Why::None && std::abs(a.speed()) < kStill)
    a.counters.heldSeconds += dt;
  const AgentMotion::Step st = a.motion->step(dt, cap, sched.stopAt);
  if (st.hardStopFrom > 0.0f)
  {
    ++a.counters.hardStops;
    a.counters.hardestStop = std::max(a.counters.hardestStop, st.hardStopFrom);
  }
  if (st.done)
  {
    a.done = true;
    if (a.toWaitSpot)
    {
      // Off the higher ones' way: its way on planned from here (a game's,
      // handed back), held there by the reservations till they've passed.
      a.toWaitSpot = false;
      a.forTraffic = true;
      a.yieldTo.clear();
      a.yieldFailed = false;
      if (a.gameLed())
      {
        a.replanning = true;
        event(Event::Kind::NeedsPlan, id, a.request, a.waitFrom);
      }
      else
        ask(id, a, Want::Trip);
    }
    else if (a.blockedAtEnd)
    {
      a.blockedAtEnd = false;
      a.blocked = true;
      event(Event::Kind::Blocked, id, a.request, 0, a.blockers);
    }
    else if (a.onTrip)
    {
      a.onTrip = false;
      a.arrived = true;
      if (a.waitingFor)
        event(Event::Kind::Resumed, id, a.request, a.waitingFor);
      a.waitingFor = 0;
      event(Event::Kind::Arrived, id, a.request);
    }
  }
  finish();
}

void Fleet::update(double deltaTime)
{
  const float dt = static_cast<float>(deltaTime);
  {
    // (The game's pace, sim seconds per real one: look-ahead deadlines.)
    const auto now = PlanScheduler::Clock::now();
    if (m_lastUpdate != PlanScheduler::Clock::time_point{})
    {
      const double real =
          std::chrono::duration<double>(now - m_lastUpdate).count();
      if (real > 1e-4 && deltaTime > 0.0)
        m_simPerReal = std::clamp(
            0.9 * m_simPerReal + 0.1 * (deltaTime / real), 0.05, 64.0);
    }
    m_lastUpdate = now;
  }
  {
    SFS_CPU_SCOPE("navigation/poll");
    for (auto& [id, a] : m_agents)
      poll(id, *a);
  }
  {
    SFS_CPU_SCOPE("navigation/claims");
    for (auto& [id, a] : m_agents)
      claim(id, *a, dt);
  }
  {
    SFS_CPU_SCOPE("navigation/traffic");
    m_traffic.schedule();
  }
  {
    SFS_CPU_SCOPE("navigation/drive");
    for (auto& [id, a] : m_agents)
      drive(id, *a, dt);
  }
  SFS_CPU_SCOPE("navigation/deadlocks");
  // The wait-for graph: who stands held by whom (not one planning its way).
  std::map<AgentId, AgentId> waits;
  for (auto& [id, a] : m_agents)
  {
    AgentId on = 0;
    if (!a->motion->handDriven() && a->want == Want::None && !a->job)
      on = m_traffic.waitsOn(id);
    if (on && !m_agents.contains(on))
      on = 0;
    a->waitedFor = on ? a->waitedFor + dt : 0.0f;
    if (on)
      waits[id] = on;
  }
  breakDeadlocks(dt, waits);
}

std::string Fleet::nameOf(AgentId id) const
{
  return m_namer ? m_namer(id) : "agent " + std::to_string(id);
}

// Who, where, and what it stands for (a deadlock's log line).
std::string Fleet::describe(AgentId id) const
{
  using Why = Traffic::Schedule::Why;
  const auto it = m_agents.find(id);
  if (it == m_agents.end())
    return nameOf(id);
  const Agent& a = *it->second;
  const Traffic::Schedule sc = m_traffic.scheduleOf(id);
  const AgentId on = m_traffic.waitsOn(id);
  char text[200];
  const Pose2 p = a.where();
  std::snprintf(
      text, sizeof(text), " at (%.1f, %.1f)", p.position.x, p.position.y);
  std::string out = nameOf(id) + text;
  if (on)
    out += std::string(" waits for ") + nameOf(on) +
           (sc.why == Why::Reserved ? "'s reservation"
            : sc.why == Why::Zone   ? "'s narrow place"
                                    : "'s body");
  else if (standing(a))
    out += a.job || a.want != Want::None ? " standing, its plan on the way"
           : a.gamePlanning || a.replanning
               ? " standing, its game planning its way"
               : " standing";
  if (a.toWaitSpot)
    out += " (on its way to a wait spot)";
  return out;
}

void Fleet::breakDeadlocks(float dt, const std::map<AgentId, AgentId>& waits)
{
  m_clock += dt;
  const auto agentOf = [&](AgentId id) -> Agent*
  {
    const auto it = m_agents.find(id);
    return it == m_agents.end() ? nullptr : it->second.get();
  };
  const auto planningWaitSpot = [](const Agent& a)
  { return a.want == Want::WaitSpot || (a.job && a.jobFor == Want::WaitSpot); };
  const auto names = [&](const std::vector<AgentId>& ids)
  {
    std::string out;
    for (const AgentId m : ids)
      out += (out.empty() ? "" : ", ") + nameOf(m);
    return out;
  };
  // Broken: none of its members waits on another of them any more (and
  // none is planning its way out).
  for (auto it = m_deadlocks.begin(); it != m_deadlocks.end();)
  {
    bool on = false;
    for (const AgentId m : it->members)
    {
      const auto w = waits.find(m);
      if (w != waits.end() &&
          std::find(it->members.begin(), it->members.end(), w->second) !=
              it->members.end())
        on = true;
      if (const Agent* a = agentOf(m); a && planningWaitSpot(*a))
        on = true;
    }
    if (on)
    {
      ++it;
      continue;
    }
    const float took = static_cast<float>(m_clock - it->since);
    ++m_deadlockStats.broken;
    m_deadlockStats.longest = std::max(m_deadlockStats.longest, took);
    char text[120];
    std::snprintf(text, sizeof(text), " broken %.1f s after they stood", took);
    LOG_INFO("Navigation: deadlock of " + names(it->members) + text +
             (it->yielder ? " (" + nameOf(it->yielder) + " gave way)"
                          : std::string()));
    for (const AgentId m : it->members)
      if (Agent* a = agentOf(m))
        a->yieldFailed = false;
    it = m_deadlocks.erase(it);
  }
  // Found: the deadlock it's part of (members shared), or a new one.
  const auto record = [&](const std::vector<AgentId>& members,
                          float stood,
                          const char* what) -> Deadlock&
  {
    for (Deadlock& d : m_deadlocks)
      for (const AgentId m : members)
        if (std::find(d.members.begin(), d.members.end(), m) != d.members.end())
        {
          for (const AgentId n : members)
            if (std::find(d.members.begin(), d.members.end(), n) ==
                d.members.end())
              d.members.push_back(n);
          return d;
        }
    Deadlock d;
    d.members = members;
    d.since = m_clock - stood;
    ++m_deadlockStats.found;
    std::string text;
    for (const AgentId m : members)
      text += (text.empty() ? "" : "; ") + describe(m);
    char stoodText[80];
    std::snprintf(stoodText, sizeof(stoodText), ", stood %.1f s", stood);
    LOG_INFO(std::string("Navigation: deadlock (") + what + "): " + text +
             stoodText);
    m_deadlocks.push_back(std::move(d));
    return m_deadlocks.back();
  };

  // Those and every one whose wait chain leads to them (a queue behind).
  const auto feeding = [&](std::vector<AgentId> group)
  {
    for (bool grew = true; grew;)
    {
      grew = false;
      for (const auto& [id, on] : waits)
        if (std::find(group.begin(), group.end(), on) != group.end() &&
            std::find(group.begin(), group.end(), id) == group.end())
        {
          group.push_back(id);
          grew = true;
        }
    }
    return group;
  };
  // Cycles: each held still by the next, all of them deadlockAfter.
  for (const std::vector<AgentId>& cycle : Traffic::cycles(waits))
  {
    float stood = kForever;
    for (const AgentId m : cycle)
      if (const Agent* a = agentOf(m))
        stood = std::min(stood, a->waitedFor);
    if (stood < m_settings.deadlockAfter)
      continue;
    const std::vector<AgentId> group = feeding(cycle);
    resolveCycle(
        record(group, stood, "each waiting on the next"), cycle, group);
  }
  // Chains ending in one standing with no path to drive that isn't moving
  // off (its plan, or its game's, long in coming): it never moves for them.
  for (const auto& [id, on] : waits)
  {
    if (waits.contains(on))
      continue; // (not the chain's end)
    const Agent* w = agentOf(id);
    const Agent* b = agentOf(on);
    if (!w || !b || w->waitedFor < 2.0f * m_settings.parkedAfter ||
        b->standingFor < 2.0f * m_settings.parkedAfter || !standing(*b) ||
        std::abs(b->speed()) >= kStill || solid(*b) || b->motion->handDriven())
      continue;
    const std::vector<AgentId> group = feeding({on});
    resolveChain(record(group, w->waitedFor, "waiting on one not moving"),
                 id,
                 on,
                 group);
  }
}

void Fleet::resolveCycle(Deadlock& d,
                         const std::vector<AgentId>& cycle,
                         const std::vector<AgentId>& group)
{
  // (A way out under way: its wait spot being planned.)
  for (const AgentId m : cycle)
  {
    const Agent& a = *m_agents.at(m);
    if (a.want == Want::WaitSpot || (a.job && a.jobFor == Want::WaitSpot))
      return;
  }
  // One on its way to a wait spot but held on it: that way out failed.
  for (const AgentId m : cycle)
    if (Agent& a = *m_agents.at(m); a.toWaitSpot)
      a.yieldFailed = true;
  // The lowest ranked yields (one at a time); failing that, the next.
  std::vector<AgentId> order = cycle;
  std::sort(order.begin(),
            order.end(),
            [&](AgentId x, AgentId y) { return m_traffic.ranksAbove(y, x); });
  for (const AgentId y : order)
  {
    Agent& a = *m_agents.at(y);
    if (a.yieldFailed || a.stuck)
      continue;
    // (Clear of all of them: the cycle and any queued behind it.)
    std::vector<AgentId> others;
    for (const AgentId m : group)
      if (m != y)
        others.push_back(m);
    if (yieldFor(y, a, others))
    {
      d.yielder = y;
      LOG_INFO("Navigation: deadlock: " + nameOf(y) +
               " yields (the lowest ranked that can), to a wait spot clear "
               "of the others' ways");
      return;
    }
  }
  // Nobody can: the lowest stands for good, a static obstacle the others
  // plan round.
  // (Logged once: the same ones forming it again (a member planned again,
  // taking the others as they stand) isn't news; the others carry on round
  // the one that stands.)
  Agent& lowest = *m_agents.at(order.front());
  if (!lowest.stuck)
  {
    lowest.stuck = true;
    std::vector<AgentId> members = cycle;
    std::sort(members.begin(), members.end());
    std::erase_if(m_hopeless,
                  [&](const auto& h)
                  { return m_clock - h.second > kHopelessFor; });
    const bool known =
        std::any_of(m_hopeless.begin(),
                    m_hopeless.end(),
                    [&](const auto& h) { return h.first == members; });
    if (!known)
    {
      m_hopeless.emplace_back(members, m_clock);
      LOG_WARN("Navigation: deadlock: nobody in it can get out of the way: " +
               nameOf(order.front()) +
               " stands for good, the others planned round it");
    }
  }
}

void Fleet::resolveChain(Deadlock& d,
                         AgentId waiter,
                         AgentId end,
                         const std::vector<AgentId>& group)
{
  Agent& b = *m_agents.at(end);
  if (b.want == Want::WaitSpot || (b.job && b.jobFor == Want::WaitSpot) ||
      b.toWaitSpot)
    return; // (on its way out)
  // Lower: it moves off to a wait spot clear of the waiter's way (not while
  // its game plans its way: that path is coming from where it stands).
  if (m_traffic.ranksAbove(waiter, end) && !b.gamePlanning && !b.yieldFailed &&
      !b.stuck &&
      yieldFor(end, b, std::vector<AgentId>(group.begin() + 1, group.end())))
  {
    d.yielder = end;
    LOG_INFO("Navigation: deadlock: " + nameOf(end) + " yields to " +
             nameOf(waiter) + ", to a wait spot clear of its way");
    return;
  }
  // Otherwise it's a static obstacle for now: the waiter plans round it.
  if (!b.stuck)
  {
    b.stuck = true;
    LOG_INFO("Navigation: deadlock: " + nameOf(end) +
             " isn't moving off: a static obstacle " + nameOf(waiter) +
             " plans round");
  }
}

bool Fleet::yieldFor(AgentId id, Agent& a, const std::vector<AgentId>& members)
{
  if (a.motion->handDriven() || a.speedCap <= 0.0f || !m_world.space)
    return false;
  AgentId from = m_traffic.waitsOn(id);
  if (!from || std::find(members.begin(), members.end(), from) == members.end())
    from = members.front();
  a.yieldTo = members;
  a.deferTo = members;
  a.deferredFor = 0.0f;
  a.waitFrom = from;
  ++a.counters.waitSpots;
  ++a.counters.deadlockYields;
  if (a.waitingFor != from)
    event(Event::Kind::Yielding, id, a.request, from);
  a.waitingFor = from;
  a.forTraffic = true;
  a.toWaitSpot = true;
  a.avoidNone();
  ask(id, a, Want::WaitSpot);
  return true;
}

Fleet::State Fleet::state(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it == m_agents.end() ? State::Idle : it->second->state;
}

Pose2 Fleet::pose(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it == m_agents.end() ? Pose2{} : it->second->where();
}

float Fleet::hitch(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it == m_agents.end() ? 0.0f : it->second->hitchNow();
}

float Fleet::speed(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it == m_agents.end() ? 0.0f : it->second->speed();
}

float Fleet::curvature(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it == m_agents.end() || !it->second->moved()
             ? 0.0f
             : it->second->motion->curvature();
}

std::optional<Pose2> Fleet::towedPose(AgentId id) const
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end())
    return std::nullopt;
  const Agent& a = *it->second;
  return a.kind->towedPose(a.spec, a.where(), a.hitchNow());
}

const AgentPath* Fleet::path(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it == m_agents.end() || !it->second->hasPath()
             ? nullptr
             : it->second->motion->path();
}

float Fleet::along(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it == m_agents.end() || !it->second->hasPath()
             ? 0.0f
             : it->second->motion->s();
}

std::vector<AgentGoal> Fleet::goals(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it == m_agents.end() ? std::vector<AgentGoal>{} : it->second->goals;
}

std::vector<Footprint> Fleet::footprint(AgentId id) const
{
  const auto it = m_agents.find(id);
  if (it == m_agents.end())
    return {};
  const Agent& a = *it->second;
  return a.kind->footprint(a.spec, a.where(), a.hitchNow());
}

std::vector<Space::Hit> Fleet::blockers(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it == m_agents.end() ? std::vector<Space::Hit>{}
                              : it->second->blockers;
}

Fleet::Counters Fleet::counters(AgentId id) const
{
  const auto it = m_agents.find(id);
  return it == m_agents.end() ? Counters{} : it->second->counters;
}

std::vector<Fleet::AgentId> Fleet::ids() const
{
  std::vector<AgentId> out;
  for (const auto& [id, a] : m_agents)
    out.push_back(id);
  return out;
}

std::vector<Fleet::Event> Fleet::takeEvents()
{
  std::vector<Event> out;
  out.swap(m_events);
  return out;
}

} // namespace sfs::nav
