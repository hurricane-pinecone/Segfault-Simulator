#pragma once

#include "engine/runtime/navigation/agentKind.h"
#include "engine/runtime/navigation/planScheduler.h"
#include "engine/runtime/navigation/traffic.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sfs::nav
{

/** m/s: an agent slower than this is standing (one value, game included). */
constexpr float kStill = 0.01f;

/**
 * Every agent of a scene, planning and moving together: the part of
 * NavigationSystem that knows nothing of entities (so tools and tests drive
 * it directly). Each agent is planned by its kind (AgentKinds) on the
 * planning scheduler it's given (PlanScheduler: none, plans run in
 * update(), the same every run) and drives its path with its kind's
 * follower. Traffic is the
 * textbook prioritised planning (Traffic): each frame every agent claims
 * its way, then in rank order (priority, then id) reserves it in space and
 * time, a lower one standing where it's clear of the higher reservations,
 * waiting for narrow places (zones) another holds, and, standing in a
 * higher one's way with nowhere clear on its path, planned to a wait spot
 * clear of them (then planned on, or handed back to its game). An agent
 * that is solid() (nothing to do; stood parkedAfter not for traffic, a stop
 * its game made; or stuck: no wait spot) is a static obstacle: in every
 * plan, and any path into it is planned again round it.
 *
 * Deadlocks: who stands waiting for whom is a wait-for graph
 * (Traffic::waitsOn()). A cycle in it (each held still by the next, for
 * deadlockAfter), or a chain ending in one standing with no path that isn't
 * moving off (parkedAfter), is a deadlock: the lowest ranked of it yields,
 * planned to a wait spot clear of every other member's claimed ground (their
 * bodies solids on the way there); with no wait spot (or held on its way to
 * it) the next lowest yields instead, one at a time, never two at once; with
 * nobody able to, the lowest is stuck (a solid the others plan round). Each
 * is logged (who, where, why, seconds stood) with how and when it broke.
 *
 * Only a finished plan is driven: never the clear way so far a search
 * hands over out of budget (an agent stands, Planning, till its plan is in;
 * one that ends short of its goal is searched again with a bigger budget).
 * "Blocked" is only reported when proven. A moving agent's next way can be
 * planned ahead (planAhead()) from where its path ends, needed by when it
 * gets there.
 */
class Fleet
{
public:
  using AgentId = uint64_t;
  enum class State
  {
    Idle,
    Planning, // standing, a plan on the way
    Moving,
    Yielding, // standing for a higher reservation or a zone, or to a wait spot
    Arrived,
    Blocked, // proven: no way to its goal
  };
  struct Event
  {
    enum class Kind
    {
      Arrived,
      Blocked,  // proven, with what blocks it
      Yielding, // began standing for `other`'s reservation or zone
      Resumed,
      // Its path was the game's (setPath) and can't go on as it was (it
      // went to a wait spot, or a static obstacle is on it): the game plans
      // it again from where it stands.
      NeedsPlan,
    };
    Kind kind = Kind::Arrived;
    AgentId agent = 0;
    uint64_t request = 0;
    AgentId other = 0;
    std::vector<Space::Hit> blockers;
  };
  struct Settings
  {
    float claimSeconds = 8.0f; // how far ahead an agent claims its way
    float claimEvery = 1.0f;   // m between its claim's samples
    // s stood still, not for traffic (a stop its game made): a solid.
    float parkedAfter = 2.0f;
    // m of its way on it claims past its next seconds: where one ranked
    // lower that meets it waits clear of; its zones are found that far.
    float wayReach = 80.0f;
    uint32_t nodeBudget = 20000, budgetCap = 1u << 20;
    uint32_t waitSpotBudget = 20000, waitSpotCap = 200000;
    // s every member of a wait-for cycle has stood held before it's a
    // deadlock (and broken).
    float deadlockAfter = 1.0f;
  };
  /** What an agent did (diagnostics, tests). */
  struct Counters
  {
    int plans = 0;      // plans asked for
    int replans = 0;    // of which for traffic (wait spots, round solids)
    int waitSpots = 0;  // times sent to a wait spot (in a higher one's way)
    int roundSolid = 0; // plans round a static obstacle on its way
    float heldSeconds = 0.0f; // standing for traffic (any reason)
    // Stops it couldn't brake for (told too late: AgentMotion::Step::
    // hardStopFrom), and the fastest it was stood from.
    int hardStops = 0;
    float hardestStop = 0.0f;
    int deadlockYields = 0; // times it yielded to break a deadlock
  };
  /** Deadlocks found and broken (diagnostics, tests). */
  struct DeadlockStats
  {
    int found = 0;
    int broken = 0;
    float longest = 0.0f; // s from its members standing to it broken
  };

  Fleet();
  /** `scheduler`: where its plans run (null: in update(), deterministic). */
  explicit Fleet(Settings settings,
                 std::shared_ptr<PlanScheduler> scheduler = nullptr);
  ~Fleet();
  Fleet(const Fleet&) = delete;
  Fleet& operator=(const Fleet&) = delete;

  const Settings& settings() const { return m_settings; }
  /** What plans from now on plan in (statics changed: a new World). */
  void setWorld(World world);
  const World& world() const { return m_world; }

  /** False if there's no agent kind of the spec's name. */
  bool add(AgentId id, AgentSpec spec, Pose2 pose, float hitch = 0.0f);
  void remove(AgentId id);
  bool has(AgentId id) const { return m_agents.contains(id); }
  void setSpec(AgentId id, AgentSpec spec);
  const AgentSpec* spec(AgentId id) const;
  void setPriority(AgentId id, int priority);
  /** Put it somewhere (standing; its plan, if any, dropped). */
  void place(AgentId id, Pose2 pose, float hitch = 0.0f);
  /**
   * Its trailer's angle while nothing here moves it (no path, not driven by
   * hand): as whoever swung the towed thing left it (hitched, put down). Its
   * footprint, a plan or a hand drive from where it stands start from it.
   */
  void setHitch(AgentId id, float hitch);

  /** Send it to a goal: a new request (the last one counts). */
  uint64_t requestGoal(AgentId id, AgentGoal goal);
  /** To the nearest of several, or anywhere `goalTest` accepts. */
  uint64_t requestGoals(AgentId id,
                        std::vector<AgentGoal> goals,
                        std::function<bool(Pose2)> goalTest = {});
  /** Drive a path made elsewhere (a field pass): claimed as any. */
  void setPath(AgentId id, std::shared_ptr<const AgentPath> path);
  /**
   * Drive no faster than this (m/s; 0 stands it where it is, braking): a
   * game's live limit (a working pace, a held pace). Infinity: none.
   */
  void setSpeedCap(AgentId id, float cap);
  /**
   * Its game is planning its next path (setPath() to come): it's about to
   * move, so never a static obstacle meanwhile (not solid(), whatever it
   * stood for). Two machines standing side by side, each planning its way
   * off, each a solid in the other's plan, searched on for ever.
   */
  void setPlanning(AgentId id, bool planning);
  /** Stop and forget its goal. */
  void cancel(AgentId id);
  /**
   * A driver takes the wheel (a player): its goal and path dropped, it's
   * moved each update by the controls last given (setControls), by its
   * kind's own motion from its spec (no faster than its speed cap), till
   * handBack() stands it where it is. Meanwhile it's solid to every other
   * agent's plans (where it goes next is anyone's guess). False: its kind
   * can't be driven by hand.
   */
  bool takeWheel(AgentId id);
  void setControls(AgentId id, const Controls& controls);
  void handBack(AgentId id);
  bool handDriven(AgentId id) const;
  /** A pair that keep their own spacing (a cart beside its combine). */
  void cooperate(AgentId a, AgentId b, bool on);

  /**
   * A plan right now on this thread (a game's own checks). `ask.world`
   * left empty: the fleet's, with the parked agents but `id` as solids.
   */
  AgentPlan planNow(AgentId id, PlanAsk ask) const;

  /**
   * Look ahead: plan its next way from where its current path ends (`ask`'s
   * from and hitch set to its last sample; `ask.world` empty: the fleet's,
   * as planNow()), needed by when it gets there (real seconds, from its
   * speed profile and how fast the game runs): a look-ahead job, shed first
   * under load. One at a time per agent (a new one replaces it). No path:
   * planned from where it stands, needed now. False: no such agent.
   */
  bool planAhead(AgentId id, PlanAsk ask);
  /** Its look-ahead plan once it's finished (taken once); none meanwhile. */
  std::optional<AgentPlan> ahead(AgentId id);
  /** Whether a look-ahead plan is on its way (or in, not taken). */
  bool planningAhead(AgentId id) const;

  /** Plans in, claims, right of way, everyone on by `deltaTime`. */
  void update(double deltaTime);

  State state(AgentId id) const;
  Pose2 pose(AgentId id) const;
  float hitch(AgentId id) const;
  float speed(AgentId id) const; // signed
  float curvature(AgentId id) const;
  std::optional<Pose2> towedPose(AgentId id) const;
  /** The path it drives (null: none) and how far along it is. */
  const AgentPath* path(AgentId id) const;
  float along(AgentId id) const;
  std::vector<AgentGoal> goals(AgentId id) const;
  std::vector<Footprint> footprint(AgentId id) const;
  /**
   * Whether it's a static obstacle in others' plans (nothing to do; stood
   * parkedAfter not for traffic; stuck with no wait spot): the one test
   * every planner world takes (the fleet's own, and a game's).
   */
  bool solid(AgentId id) const;
  /** Blocked: what blocks it. */
  std::vector<Space::Hit> blockers(AgentId id) const;
  Counters counters(AgentId id) const;
  std::vector<AgentId> ids() const;
  /** The events since the last call. */
  std::vector<Event> takeEvents();
  const Traffic& traffic() const { return m_traffic; }
  DeadlockStats deadlockStats() const { return m_deadlockStats; }
  /** How agents are named in its log lines (default "agent <id>"). */
  void setNamer(std::function<std::string(AgentId)> namer)
  {
    m_namer = std::move(namer);
  }

private:
  struct Job;
  struct Agent;
  enum class Want
  {
    None,
    Trip,     // its goal, from where it stands
    WaitSpot, // off the reservations ranked above it
  };
  void ask(AgentId id, Agent& a, Want want);
  void submit(AgentId id, Agent& a);
  std::shared_ptr<Job> start(AgentId id,
                             const Agent& a,
                             PlanAsk ask,
                             const char* what,
                             PlanScheduler::Clock::time_point deadline,
                             bool lookAhead);
  static std::optional<AgentPlan> finished(PlanScheduler* scheduler, Job& job);
  void poll(AgentId id, Agent& a);
  void take(AgentId id, Agent& a, const AgentPlan& plan);
  // A way it planned on its own (a wait spot's, a trip's) that would hold
  // it standing in the way of one ranked above it (Traffic::crossing()):
  // planned again round that one's way (Again), up to kAvoidTries times
  // (then Held: its first way is driven).
  enum class Round
  {
    Clear,
    Again,
    Held,
  };
  Round planRound(AgentId id, Agent& a, const AgentPlan& plan);
  void claim(AgentId id, Agent& a, float dt);
  void drive(AgentId id, Agent& a, float dt);
  World planWorld(AgentId except, const std::vector<Footprint>& more) const;
  bool standing(const Agent& a) const;
  bool parked(const Agent& a) const;
  bool still(const Agent& a) const;
  bool solid(const Agent& a) const;
  std::vector<Traffic::Zone> zones(Agent& a, float s, std::size_t from);
  struct Deadlock
  {
    std::vector<AgentId> members; // (a cycle in waiting order, or a chain)
    double since = 0.0;           // when its members began standing
    AgentId yielder = 0;
  };
  void breakDeadlocks(float dt, const std::map<AgentId, AgentId>& waits);
  void resolveCycle(Deadlock& d,
                    const std::vector<AgentId>& cycle,
                    const std::vector<AgentId>& group);
  void resolveChain(Deadlock& d,
                    AgentId waiter,
                    AgentId end,
                    const std::vector<AgentId>& group);
  bool yieldFor(AgentId id, Agent& a, const std::vector<AgentId>& members);
  std::string nameOf(AgentId id) const;
  std::string describe(AgentId id) const;
  void event(Event::Kind kind,
             AgentId id,
             uint64_t request,
             AgentId other = 0,
             std::vector<Space::Hit> blockers = {})
  {
    m_events.push_back({kind, id, request, other, std::move(blockers)});
  }

  Settings m_settings;
  World m_world;
  std::map<AgentId, std::unique_ptr<Agent>> m_agents;
  Traffic m_traffic;
  std::shared_ptr<PlanScheduler> m_scheduler;
  std::vector<Event> m_events;
  // Sim seconds per real second (the game's time scale), measured: what a
  // path's remaining time is in real time.
  double m_simPerReal = 1.0;
  PlanScheduler::Clock::time_point m_lastUpdate{};
  uint64_t m_nextRequest = 1;
  double m_clock = 0.0; // sim seconds updated
  std::vector<Deadlock> m_deadlocks;
  // Deadlocks nobody in could get out of (their members, sorted), and when
  // that was logged: logged once, not each time it forms again.
  std::vector<std::pair<std::vector<AgentId>, double>> m_hopeless;
  DeadlockStats m_deadlockStats;
  std::function<std::string(AgentId)> m_namer;
};

} // namespace sfs::nav
