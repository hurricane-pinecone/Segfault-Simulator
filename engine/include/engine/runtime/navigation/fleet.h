#pragma once

#include "engine/runtime/navigation/agentKind.h"
#include "engine/runtime/navigation/traffic.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace sfs
{
class WorkerPool;
}

namespace sfs::nav
{

/** m/s: an agent slower than this is standing (one value, game included). */
constexpr float kStill = 0.01f;

/**
 * Every agent of a scene, planning and moving together: the part of
 * NavigationSystem that knows nothing of entities (so tools and tests drive
 * it directly). Each agent is planned by its kind (AgentKinds) on worker
 * threads and drives its path with its kind's follower. Traffic is the
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
 * Out of budget a plan hands over the clear way so far, which is driven
 * while the search goes on; "Blocked" is only reported when proven.
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
      Partial,  // driving the clear part while it searches on
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
    unsigned threads = 2;      // planning threads (0: plans run in update())
    float claimSeconds = 8.0f; // how far ahead an agent claims its way
    float claimEvery = 1.0f;   // m between its claim's samples
    // s stood still, not for traffic (a stop its game made): a solid.
    float parkedAfter = 2.0f;
    // m of its way on it claims past its next seconds: where one ranked
    // lower that meets it waits clear of; its zones are found that far.
    float wayReach = 80.0f;
    uint32_t nodeBudget = 20000, budgetCap = 1u << 20;
    uint32_t waitSpotBudget = 20000, waitSpotCap = 200000;
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
  };

  Fleet();
  explicit Fleet(Settings settings);
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
  void poll(AgentId id, Agent& a);
  void take(AgentId id, Agent& a, const AgentPlan& plan, bool final);
  void claim(AgentId id, Agent& a, float dt);
  void drive(AgentId id, Agent& a, float dt);
  World planWorld(AgentId except, const std::vector<Footprint>& more) const;
  bool standing(const Agent& a) const;
  bool parked(const Agent& a) const;
  bool still(const Agent& a) const;
  bool solid(const Agent& a) const;
  std::vector<Traffic::Zone> zones(Agent& a, float s, std::size_t from);
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
  std::unique_ptr<WorkerPool> m_pool;
  std::vector<Event> m_events;
  uint64_t m_nextRequest = 1;
};

} // namespace sfs::nav
