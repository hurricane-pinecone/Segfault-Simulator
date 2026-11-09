#include "engine/runtime/navigation/fleet.h"

#include "engine/core/jobs/workerPool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <span>

namespace sfs::nav
{

namespace
{
constexpr float kForever = std::numeric_limits<float>::infinity();
} // namespace

// One plan on its way: the latest it has (a partial while it searches on).
struct Fleet::Job
{
  std::mutex mutex;
  AgentPlan latest;
  uint64_t seq = 0;
  bool final = false;
  std::atomic<bool> cancel{false};
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
  uint64_t seen = 0;
  float capScale = 1.0f; // (a plan out of its cap: the next one's bigger)

  uint64_t pathId = 0;
  bool done = true;           // at its path's end (or none)
  bool onTrip = false;        // its path ends at its goal
  bool partial = false;       // the clear part only: plan on at its end
  bool drivenPartial = false; // (its job's clear part followed)
  bool toWaitSpot = false;    // driving to a wait spot (or planning it)
  bool stuck = false;         // in a higher one's way, no wait spot: solid
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
  std::vector<Space::Hit> blockers;
  Counters counters;

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

Fleet::Fleet(Settings settings) : m_settings(settings)
{
  if (m_settings.threads > 0)
    m_pool = std::make_unique<WorkerPool>(m_settings.threads);
}

Fleet::~Fleet()
{
  for (auto& [id, a] : m_agents)
    if (a->job)
      a->job->cancel = true;
  m_pool.reset(); // (waits for the running plans)
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
  if (it->second->job)
    it->second->job->cancel = true;
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
  a.partial = a.onTrip = a.toWaitSpot = a.stuck = false;
  a.blockers.clear();
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
  if (a.job)
    a.job->cancel = true;
  a.job = nullptr;
  a.want = Want::None;
  // (Kept: a game that cancels while it plans the one handed back (NeedsPlan)
  // still has work for it; its next path or goal ends that.)
  a.goals.clear();
  a.goalTest = {};
  a.onTrip = a.partial = a.toWaitSpot = a.stuck = false;
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

void Fleet::ask(AgentId, Agent& a, Want want)
{
  if (a.job)
    a.job->cancel = true;
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
    std::vector<Footprint> solids;
    for (const Footprint& f : m_traffic.heldBesides(id))
      if (!hits(here, {f}))
        solids.push_back(f);
    if (const auto o = m_agents.find(a.waitFrom); o != m_agents.end())
      for (const Footprint& f : o->second->kind->footprint(
               o->second->spec, o->second->where(), o->second->hitchNow()))
        solids.push_back(f);
    ask.world = planWorld(id, solids);
    std::vector<Footprint> reserved = m_traffic.reservedAbove(id);
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
    ask.world = planWorld(id, {});
    ask.goals = a.goals;
    ask.goalTest = a.goalTest;
    ask.mode = a.goals.empty() ? 0 : a.goals.front().mode;
    ask.nodeBudget = m_settings.nodeBudget;
    ask.budgetCap = static_cast<uint32_t>(
        std::min(4.0e9, double(m_settings.budgetCap) * a.capScale));
  }
  auto job = std::make_shared<Job>();
  a.job = job;
  a.jobFor = a.want;
  a.want = Want::None;
  a.seen = 0;
  a.drivenPartial = false;
  ++a.counters.plans;
  if (a.forTraffic)
    ++a.counters.replans;
  a.forTraffic = false;
  auto run = [job, kind = a.kind, spec = a.spec, ask]() mutable
  {
    ask.cancel = &job->cancel;
    AgentPlan r = kind->plan(spec,
                             ask,
                             [&job](const AgentPlan& p)
                             {
                               std::lock_guard lock(job->mutex);
                               job->latest = p;
                               ++job->seq;
                             });
    std::lock_guard lock(job->mutex);
    job->latest = std::move(r);
    job->final = true;
  };
  if (m_pool)
    m_pool->submit(std::move(run));
  else
    run();
}

void Fleet::poll(AgentId id, Agent& a)
{
  if (!a.job)
    return;
  AgentPlan plan;
  bool final = false, fresh = false;
  {
    std::lock_guard lock(a.job->mutex);
    final = a.job->final;
    fresh = a.job->seq > a.seen;
    if (final || fresh)
      plan = a.job->latest;
    a.seen = a.job->seq;
  }
  if (final)
    a.job = nullptr;
  if (final || fresh)
    take(id, a, plan, final);
}

void Fleet::take(AgentId id, Agent& a, const AgentPlan& plan, bool final)
{
  const bool usable = plan.path && plan.path->length() > 0.05f;
  if (!final)
  {
    // The clear part so far: driven while it searches on.
    if (a.jobFor == Want::Trip && usable && plan.path->length() > 0.5f &&
        standing(a))
    {
      a.follow(plan.path);
      a.partial = true;
      a.drivenPartial = true;
      a.onTrip = false;
      event(Event::Kind::Partial, id, a.request);
    }
    return;
  }
  if (a.jobFor == Want::WaitSpot)
  {
    // (No wait spot, or it's as clear as it gets where it stands: it can't
    // get out of the way; a static obstacle the others plan round.)
    if (!usable)
    {
      a.toWaitSpot = false;
      a.stuck = true;
      return;
    }
    a.follow(plan.path);
    a.toWaitSpot = true;
    return;
  }
  if (plan.state == PlanState::Blocked)
  {
    a.blockers = plan.blockers;
    a.partial = a.onTrip = false;
    if (standing(a))
    {
      a.blocked = true;
      event(Event::Kind::Blocked, id, a.request, 0, a.blockers);
    }
    else
      a.blockedAtEnd = true;
    return;
  }
  if (!usable)
  {
    if (plan.state == PlanState::Found)
    {
      // (There already.)
      a.arrived = true;
      event(Event::Kind::Arrived, id, a.request);
      return;
    }
    // Out of its cap with nothing clear yet: on with a bigger one.
    if (a.capScale < 8.0f)
    {
      a.capScale *= 2.0f;
      ask(id, a, Want::Trip);
    }
    return;
  }
  const bool found = plan.state == PlanState::Found;
  if (a.drivenPartial)
  {
    // It drove the clear part: on along the whole way from where it is, if
    // that passes there (it does unless the search changed its mind).
    a.drivenPartial = false;
    const Pose2 at = a.where();
    const int gear = a.speed() < 0.0f ? -1 : 1;
    std::optional<float> s;
    float best = 0.05f;
    for (const AgentPath::Sample& p : plan.path->samples)
    {
      const float d = glm::length(p.pose.position - at.position);
      if (d < best && (p.gear == gear || standing(a)) &&
          std::abs(wrapAngle(p.pose.heading - at.heading)) < 0.03f)
      {
        best = d;
        s = p.s;
      }
    }
    if (!s)
    {
      // (Not there: planned on from the clear part's end.)
      if (standing(a))
        ask(id, a, Want::Trip);
      else
        a.partial = true;
      return;
    }
    a.follow(plan.path, *s);
  }
  else
    a.follow(plan.path);
  a.onTrip = found;
  a.partial = !found;
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
  c.body = a.kind->footprint(a.spec, a.where(), a.hitchNow());
  c.standing = standing(a);
  c.solid = solid(a);
  c.leaving = a.toWaitSpot ? a.waitFrom : 0;
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
    a.stuck = false; // (moving: out of the way after all)
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
  if (sched.inTheWay && !a.toWaitSpot && !a.stuck && a.speedCap > 0.0f)
  {
    // In a higher one's way with nowhere clear on its path: to a wait spot.
    a.waitFrom = sched.by;
    ++a.counters.waitSpots;
    if (a.waitingFor != sched.by)
      event(Event::Kind::Yielding, id, a.request, sched.by);
    a.waitingFor = sched.by;
    a.forTraffic = true;
    a.toWaitSpot = true;
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
    else if (a.partial)
    {
      a.partial = false;
      if (!a.job)
        ask(id, a, Want::Trip); // (its search ended short: on from here)
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
  for (auto& [id, a] : m_agents)
    poll(id, *a);
  for (auto& [id, a] : m_agents)
    claim(id, *a, dt);
  m_traffic.schedule();
  for (auto& [id, a] : m_agents)
    drive(id, *a, dt);
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
