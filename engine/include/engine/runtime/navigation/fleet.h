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

/**
 * Every agent of a scene, planning and moving together: the part of
 * NavigationSystem that knows nothing of entities (so tools and tests drive
 * it directly). Each agent is planned by its kind (AgentKinds) on worker
 * threads, drives its path with its kind's follower, and claims its next
 * seconds in Traffic, whose advice it takes: the one giving way holds short,
 * clears out of the other's way (then waits till its own way on is clear of
 * the other's claim) or, held by one that has parked, plans round it. Only
 * the one giving way ever re-plans. An agent standing a while (parkedAfter,
 * whether or not it still holds a path) is a solid in everyone else's plans;
 * a moving one never is. The one with the right of way waits for the other's
 * body on its way only until that one has parked there (roundAfter): then it
 * plans round it, so a pair never both hold (a mutual hold).
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
    Yielding, // giving way (holding short, clearing out, waiting)
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
      Yielding, // began giving way to `other`
      Resumed,
      // Its path was the game's (setPath) and can't go on as it was (it
      // cleared out of another's way, or one parked on it): the game plans
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
    unsigned threads = 2;        // planning threads (0: plans run in update())
    float claimSeconds = 8.0f;   // how far ahead an agent claims its way
    float claimEvery = 1.0f;     // m between its claim's samples
    float parkedAfter = 2.0f;    // s still (path or not): a solid to others
    float roundAfter = 5.0f;     // s held by a parked one: plan round it
    float clearAhead = 15.0f;    // m of its way on clear of the other first
    float clearOutReach = 80.0f; // m of the other's way a clear-out leaves
    uint32_t nodeBudget = 20000, budgetCap = 1u << 20;
    uint32_t clearOutBudget = 20000, clearOutCap = 200000;
  };
  /** What an agent did (diagnostics, tests). */
  struct Counters
  {
    int plans = 0;       // plans asked for
    int replans = 0;     // of which for giving way (clear-outs, round)
    int clearOuts = 0;   // times told to clear out
    int roundParked = 0; // plans round one parked on its way
    float heldSeconds = 0.0f, waitSeconds = 0.0f;
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
  /** Stop and forget its goal. */
  void cancel(AgentId id);
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
    ClearOut, // off another's claim
  };
  void ask(AgentId id, Agent& a, Want want);
  void submit(AgentId id, Agent& a);
  void poll(AgentId id, Agent& a);
  void take(AgentId id, Agent& a, const AgentPlan& plan, bool final);
  void claim(AgentId id, Agent& a);
  void drive(AgentId id, Agent& a, float dt);
  World planWorld(AgentId except, const std::vector<Footprint>& more) const;
  bool standing(const Agent& a) const;
  bool parked(const Agent& a) const;
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
  double m_now = 0.0;
};

} // namespace sfs::nav
