#include "engine/runtime/navigation/fleet.h"

#include "engine/core/jobs/workerPool.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>

namespace sfs::nav
{

namespace
{
constexpr float kForever = std::numeric_limits<float>::infinity();
constexpr float kStill = 0.01f; // m/s: standing
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

  Want want = Want::None; // a plan wanted once it stands
  bool forYield = false;  // (that plan is for giving way)
  std::shared_ptr<Job> job;
  Want jobFor = Want::None;
  uint64_t seen = 0;
  float capScale = 1.0f; // (a plan out of its cap: the next one's bigger)

  uint64_t pathId = 0;
  bool done = true;           // at its path's end (or none)
  bool onTrip = false;        // its path ends at its goal
  bool partial = false;       // the clear part only: plan on at its end
  bool drivenPartial = false; // (its job's clear part followed)
  bool clearing = false;      // driving off another's claim
  bool clearPartial = false;
  bool afterClear = false;
  bool blockedAtEnd = false;
  bool arrived = false, blocked = false;
  AgentId clearedFrom = 0;
  std::shared_ptr<const AgentPath> waiting; // its way on, after a clear-out

  float speedCap = std::numeric_limits<float>::infinity(); // (the game's)
  float heldFor = 0.0f, standingFor = 0.0f;
  bool yielding = false;
  std::vector<Space::Hit> blockers;
  Counters counters;

  // Driving a path the game made (setPath): no goal of its own to plan.
  bool gameLed() const { return goals.empty() && !goalTest; }
  bool hasPath() const
  {
    return motion->path() && motion->path()->samples.size() >= 2;
  }
  Pose2 where() const { return hasPath() ? motion->pose() : pose; }
  float hitchNow() const { return hasPath() ? motion->hitch() : hitch; }
  float speed() const { return hasPath() ? motion->speed() : 0.0f; }
  void follow(std::shared_ptr<const AgentPath> path, float fromS = 0.0f)
  {
    const float v = std::abs(speed());
    pose = where();
    hitch = hitchNow();
    motion->setPath(std::move(path), v, fromS);
    ++pathId;
    done = !hasPath();
  }
  void stand()
  {
    pose = where();
    hitch = hitchNow();
    motion->setPath(nullptr, 0.0f);
    ++pathId;
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
  Agent& a = *it->second;
  a.goals = std::move(goals);
  a.goalTest = std::move(goalTest);
  a.request = m_nextRequest++;
  a.arrived = a.blocked = a.blockedAtEnd = false;
  a.clearing = a.afterClear = a.partial = a.onTrip = false;
  a.waiting = nullptr;
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
  Agent& a = *it->second;
  cancel(id);
  a.request = m_nextRequest++;
  a.follow(std::move(path));
  a.onTrip = true;
}

void Fleet::setSpeedCap(AgentId id, float cap)
{
  if (const auto it = m_agents.find(id); it != m_agents.end())
    it->second->speedCap = std::max(cap, 0.0f);
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
  a.goals.clear();
  a.goalTest = {};
  a.onTrip = a.partial = a.clearing = a.afterClear = false;
  a.blocked = a.blockedAtEnd = a.arrived = false;
  a.waiting = nullptr;
  a.yielding = false;
  if (a.hasPath() && !a.done)
    a.stand();
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
    if (id != except && parked(*a))
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

// Stood still a while, with or without a path: a held one counts (one held
// on another's way that never moves is planned round, never waited on).
bool Fleet::parked(const Agent& a) const
{
  return a.standingFor >= m_settings.parkedAfter;
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
  if (a.want == Want::ClearOut)
  {
    // Anywhere its body is clear of the other's claim (and may wait), the
    // other's body a solid meanwhile.
    std::vector<Footprint> body;
    if (const auto o = m_agents.find(a.clearedFrom); o != m_agents.end())
      body = o->second->kind->footprint(
          o->second->spec, o->second->where(), o->second->hitchNow());
    ask.world = planWorld(id, body);
    // (Its whole way on, not just its claim: the claim moves on with it
    // while this one clears out.)
    std::vector<Footprint> claimed = m_traffic.claimed(a.clearedFrom);
    if (const auto o = m_agents.find(a.clearedFrom);
        o != m_agents.end() && !standing(*o->second))
    {
      const Agent& b = *o->second;
      const float s0 = b.motion->s();
      long last = -1;
      const std::vector<AgentPath::Sample>& way = b.motion->path()->samples;
      for (const AgentPath::Sample& p : way)
        if (p.s >= s0 && p.s - s0 <= m_settings.clearOutReach &&
            (static_cast<long>(p.s / m_settings.claimEvery) != last ||
             &p == &way.back())) // (its end too: where it will stand)
        {
          last = static_cast<long>(p.s / m_settings.claimEvery);
          for (const Footprint& f : b.kind->footprint(b.spec, p.pose, p.hitch))
            claimed.push_back(f);
        }
    }
    // (A little more room than Traffic asks: it parks clear, not on the
    // edge of the other's way.)
    const float margin = m_traffic.margin() + 0.25f;
    std::shared_ptr<const AgentKind> kind = a.kind;
    std::shared_ptr<const CostLayer> cost = m_world.cost;
    ask.goalTest = [claimed, margin, kind, spec = a.spec, cost](Pose2 p)
    {
      if (cost && cost->noWait(p.position))
        return false;
      for (const Footprint& f : kind->footprint(spec, p, 0.0f))
        for (const Footprint& q : claimed)
          if (overlaps(f, q, margin))
            return false;
      return true;
    };
    ask.nodeBudget = m_settings.clearOutBudget;
    ask.budgetCap = m_settings.clearOutCap;
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
  if (a.forYield)
    ++a.counters.replans;
  a.forYield = false;
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
    if (a.jobFor == Want::Trip && !a.afterClear && usable &&
        plan.path->length() > 0.5f && standing(a))
    {
      a.follow(plan.path);
      a.partial = true;
      a.drivenPartial = true;
      a.onTrip = false;
      event(Event::Kind::Partial, id, a.request);
    }
    return;
  }
  if (plan.state == PlanState::Blocked)
  {
    if (a.jobFor == Want::ClearOut)
    {
      a.clearing = false;
      m_traffic.cannotClear(id, a.clearedFrom);
      return;
    }
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
      if (a.jobFor == Want::ClearOut)
      {
        // (Clear where it stands by its own test, yet in the other's way by
        // Traffic's: it can't clear out further; the other gives way.)
        a.clearing = false;
        a.yielding = false;
        m_traffic.cannotClear(id, a.clearedFrom);
      }
      else if (a.afterClear)
      {
        a.afterClear = false;
        a.arrived = true;
        event(Event::Kind::Arrived, id, a.request);
      }
      else
      {
        a.arrived = true;
        event(Event::Kind::Arrived, id, a.request);
      }
      return;
    }
    // Out of its cap with nothing clear yet: on with a bigger one.
    if (a.jobFor == Want::ClearOut)
    {
      m_traffic.cannotClear(id, a.clearedFrom);
      return;
    }
    if (a.capScale < 8.0f)
    {
      a.capScale *= 2.0f;
      ask(id, a, Want::Trip);
    }
    return;
  }
  const bool found = plan.state == PlanState::Found;
  if (a.jobFor == Want::ClearOut)
  {
    a.follow(plan.path);
    a.clearing = true;
    a.clearPartial = !found; // (the clear part only: on from its end)
    return;
  }
  if (a.afterClear)
  {
    a.waiting = plan.path;
    a.onTrip = found;
    a.partial = !found;
    return;
  }
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

void Fleet::claim(AgentId id, Agent& a)
{
  Traffic::Claim c;
  c.priority = a.priority;
  c.path = a.pathId;
  c.body = a.kind->footprint(a.spec, a.where(), a.hitchNow());
  c.standing = standing(a);
  c.parked = parked(a);
  if (!c.standing)
  {
    c.s = a.motion->s();
    const std::vector<AgentPath::Sample>& p = a.motion->path()->samples;
    const std::size_t i0 = std::min(a.motion->index(), p.size() - 1);
    const float t0 = p[i0].time;
    // (Samples on a fixed grid along the path, the same whichever frame
    // it's claimed in.)
    long last = -1;
    for (std::size_t k = i0;
         k < p.size() && p[k].time - t0 < m_settings.claimSeconds;
         ++k)
      if (static_cast<long>(p[k].s / m_settings.claimEvery) != last ||
          k + 1 == p.size())
      {
        last = static_cast<long>(p[k].s / m_settings.claimEvery);
        c.ahead.push_back({p[k].s,
                           p[k].time - t0 - 0.5f,
                           p[k].time - t0 + 0.5f,
                           a.kind->footprint(a.spec, p[k].pose, p[k].hitch)});
      }
  }
  m_traffic.update(id, std::move(c), m_now);
}

void Fleet::drive(AgentId id, Agent& a, float dt)
{
  const auto finish = [&]
  {
    a.state = a.blocked                                        ? State::Blocked
              : a.arrived                                      ? State::Arrived
              : a.clearing || a.afterClear                     ? State::Yielding
              : (a.want != Want::None || a.job) && standing(a) ? State::Planning
              : a.yielding && !standing(a)                     ? State::Yielding
              : !standing(a)                                   ? State::Moving
                                                               : State::Idle;
  };
  a.standingFor = std::abs(a.speed()) < kStill ? a.standingFor + dt : 0.0f;

  if (a.want != Want::None)
  {
    // Stand first, then plan from there.
    if (!standing(a) && std::abs(a.speed()) >= kStill)
      a.motion->step(dt, 0.0f, kForever);
    else if (m_world.space) // (no world yet: it waits for one)
      submit(id, a);
    finish();
    return;
  }
  if (standing(a))
  {
    if (a.afterClear && a.waiting)
    {
      // Off the other's way: on once its way on is clear of the other's
      // claim.
      bool clear = true;
      for (const AgentPath::Sample& p : a.waiting->samples)
      {
        if (p.s > m_settings.clearAhead)
          break;
        if (!m_traffic.clearOf(
                a.clearedFrom, a.kind->footprint(a.spec, p.pose, p.hitch)))
        {
          clear = false;
          break;
        }
      }
      const auto o = m_agents.find(a.clearedFrom);
      if (!clear && o != m_agents.end() && parked(*o->second))
      {
        // (The other has parked on its way on: a solid now, planned round.)
        a.waiting = nullptr;
        a.forYield = true;
        ask(id, a, Want::Trip);
      }
      else if (clear)
      {
        a.follow(a.waiting);
        a.waiting = nullptr;
        a.afterClear = false;
        a.yielding = false;
        event(Event::Kind::Resumed, id, a.request, a.clearedFrom);
      }
    }
    finish();
    return;
  }

  const Traffic::Advice adv = m_traffic.advise(id);
  float cap = kForever, stop = kForever;
  using Kind = Traffic::Advice::Kind;
  if (adv.kind == Kind::ClearOut && !a.clearing)
  {
    a.clearedFrom = adv.other;
    ++a.counters.clearOuts;
    a.forYield = true;
    if (!a.yielding)
      event(Event::Kind::Yielding, id, a.request, adv.other);
    a.yielding = true;
    ask(id, a, Want::ClearOut);
    a.motion->step(dt, 0.0f, kForever);
    finish();
    return;
  }
  if (adv.kind == Kind::HoldAt)
  {
    stop = adv.s;
    a.counters.heldSeconds += dt;
    a.heldFor += dt;
    const auto o = m_agents.find(adv.other);
    if (o != m_agents.end() && parked(*o->second) &&
        a.heldFor > m_settings.roundAfter && !a.clearing)
    {
      // Held by one that has parked: a solid now, planned round (the
      // game's own path: the game plans it again).
      a.heldFor = 0.0f;
      ++a.counters.roundParked;
      if (a.gameLed())
      {
        a.stand();
        event(Event::Kind::NeedsPlan, id, a.request, adv.other);
        finish();
        return;
      }
      ask(id, a, Want::Trip);
      a.motion->step(dt, 0.0f, kForever);
      finish();
      return;
    }
  }
  else if (adv.kind != Kind::Wait)
    a.heldFor = 0.0f;
  if (adv.kind == Kind::Wait)
  {
    cap = 0.0f;
    a.counters.waitSeconds += dt;
    a.heldFor += dt;
    const auto o = m_agents.find(adv.other);
    if (o != m_agents.end() && parked(*o->second) &&
        a.heldFor > m_settings.roundAfter)
    {
      // (One parked on its way: a solid now, planned round, A6. Parked
      // includes one giving way to it that is held there (its path kept, a
      // speed cap of 0, a clear-out it can't drive): waiting on it would be a
      // mutual hold, both standing for good.)
      a.heldFor = 0.0f;
      ++a.counters.roundParked;
      if (a.gameLed())
      {
        a.stand();
        event(Event::Kind::NeedsPlan, id, a.request, adv.other);
        finish();
        return;
      }
      ask(id, a, Want::Trip);
      a.motion->step(dt, 0.0f, kForever);
      finish();
      return;
    }
  }
  if (adv.kind != Kind::Go && !a.yielding)
  {
    a.yielding = true;
    event(Event::Kind::Yielding, id, a.request, adv.other);
  }
  else if (adv.kind == Kind::Go && a.yielding && !a.clearing)
  {
    a.yielding = false;
    event(Event::Kind::Resumed, id, a.request);
  }
  for (const AgentGoal& g : a.goals)
    cap = std::min(cap, g.speedCap);
  cap = std::min(cap, a.speedCap);
  const AgentMotion::Step st = a.motion->step(dt, cap, stop);
  if (st.done)
  {
    a.done = true;
    if (a.clearing)
    {
      a.clearing = false;
      a.forYield = true;
      if (a.clearPartial)
        ask(id, a, Want::ClearOut); // (not off its way yet)
      else if (a.gameLed())
      {
        // (Off the other's way; its way on is the game's to plan.)
        a.yielding = false;
        event(Event::Kind::NeedsPlan, id, a.request, a.clearedFrom);
      }
      else
      {
        a.afterClear = true;
        ask(id, a, Want::Trip);
      }
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
      a.yielding = false;
      event(Event::Kind::Arrived, id, a.request);
    }
  }
  finish();
}

void Fleet::update(double deltaTime)
{
  m_now += deltaTime;
  const float dt = static_cast<float>(deltaTime);
  for (auto& [id, a] : m_agents)
    poll(id, *a);
  for (auto& [id, a] : m_agents)
    claim(id, *a);
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
  return it == m_agents.end() || !it->second->hasPath()
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
