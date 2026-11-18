#pragma once

#include "engine/core/components/navAgentComponent.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/moduleSettings.h"
#include "engine/core/ecs/system.h"
#include "engine/runtime/navigation/agentKind.h"
#include "engine/runtime/navigation/coarseGraph.h"
#include "engine/runtime/navigation/costLayer.h"
#include "engine/runtime/navigation/fleet.h"
#include "engine/runtime/navigation/geometry.h"
#include "engine/runtime/navigation/laneGraph.h"
#include "engine/runtime/navigation/planScheduler.h"
#include "engine/runtime/navigation/space.h"
#include "engine/runtime/navigation/vehicle/vehicleKind.h"

#include <glm/glm/ext/vector_float2.hpp>
#include <glm/glm/ext/vector_float3.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sfs
{

/**
 * Navigation for AI agents (NavAgentComponent): it plans their ways round
 * the scene's colliders on its planning scheduler (nav::PlanScheduler: one
 * worker pool, deadline first, a fair turn per agent), drives them along and
 * settles who gives way when they meet. A game sends an agent somewhere
 * (requestGoal) and hears how it went (nav::AgentEvent, sent with the agent
 * as it happens); it never moves the agent itself meanwhile: the system
 * writes the agent's transform (and its trailer's) each update.
 *
 * What's solid comes from PhysicsSystem3D: a frozen copy of the static
 * colliders (PhysicsSystem3D::snapshot()), rebuilt when its revision
 * changes, with the coarse graph long trips take their corridor from (built
 * on the scheduler). Agents leave themselves out; one with nothing to do (or
 * held by its game) is solid to the others' plans, one moving never is (they
 * keep out of each other's reservations by rank instead: see nav::Fleet and
 * nav::Traffic). A game adds what ground costs (setCostLayer:
 * flowerbeds, worn tracks; layers multiply) and who goes first
 * (setPriorityProvider: a working machine).
 *
 * An agent of kind "vehicle" also needs a VehicleComponent: its motion is
 * derived from that's VehicleSpec and its rig (setRigProvider: the mass it
 * moves, the grip under it, a measured wheelbase). Without one it's left
 * out, with an error logged.
 *
 * Agent kinds come from nav::AgentKinds ("vehicle" built in), path searches
 * from each kind's registry (the vehicle's nav::vehicle::searches(),
 * "hybridAStar" built in), picked by the component's fields.
 *
 * What the game keeps on an agent's NavAgentComponent it writes there: what
 * it tows (hitch), how fast it may go (speedCap) and whether it's an agent
 * now (active). The system reads them at its update and before every order
 * that plans. Game code gives orders and reads agents through its
 * navigation() facility (facility::Navigation), never by looking it up.
 *
 * Debug views (paths, claims, what blocks a way) are drawn with debugDraw()
 * while debugFeatures::enabled(), switched on in settings(). A scene lists it
 * by name ("NavigationSystem") after "PhysicsSystem3D".
 */
class NavigationSystem : public System
{
public:
  NavigationSystem() = default;
  NavigationSystem(const NavigationSystem&) = delete;
  ~NavigationSystem() override;
  NavigationSystem& operator=(const NavigationSystem&) = delete;
  /** Its look-ahead plan once finished (nav::Fleet::ahead()). */
  std::optional<nav::AgentPlan> ahead(const Entity& agent);
  /** Stop and forget its goal. */
  void cancel(const Entity& agent);
  const std::vector<nav::CoarseGraph::Edge>& coarseEdges() const
  {
    return m_coarseEdges;
  }
  /** A pair that keep their own spacing (a truck beside its loader). */
  void cooperate(const Entity& a, const Entity& b, bool on);
  /** Everything about the agents (paths, claims, counters). */
  const nav::Fleet& fleet() const { return *m_fleet; }
  void handBack(const Entity& agent);
  bool handDriven(const Entity& agent) const;
  std::shared_ptr<const nav::LaneGraph>
  laneGraph(const std::string& name) const;
  /** Plan an agent's next way ahead (nav::Fleet::planAhead()). */
  bool planAhead(const Entity& agent, nav::PlanAsk ask);
  /**
   * Plan now on this thread: only for a check that truly needs its answer
   * this frame; anything else goes through scheduler().
   */
  nav::AgentPlan planNow(const Entity& agent, nav::PlanAsk ask);
  /** Read an agent's colliders and fields again (after changing them). */
  void refresh(const Entity& agent);
  /** Send an agent somewhere: a new request (the last one counts), its id. */
  uint64_t requestGoal(const Entity& agent, const nav::AgentGoal& goal);
  /** To the nearest of several, or anywhere `goalTest` accepts. */
  uint64_t requestGoals(const Entity& agent,
                        std::vector<nav::AgentGoal> goals,
                        std::function<bool(nav::Pose2)> goalTest = {});
  /**
   * THE planning scheduler: every path job of the scene (the agents', the
   * coarse graph, a game's own) runs on its one pool.
   */
  nav::PlanScheduler& scheduler() { return *m_scheduler; }
  /**
   * The game's own edges on the coarse graph (its worn tracks: cheaper to
   * route along, so long trips' corridors take them), replacing the last
   * ones: put on the graph as it is and on every one built after, on the
   * scheduler (nav::CoarseGraph::withEdges(): only the cells round what came
   * or went priced again). Plans holding the old graph finish on it.
   */
  void setCoarseEdges(std::vector<nav::CoarseGraph::Edge> edges);
  void setControls(const Entity& agent, const nav::Controls& controls);
  /** A ground cost by name (null: removed); all of them multiply. */
  void setCostLayer(const std::string& name,
                    std::shared_ptr<const nav::CostLayer> layer);
  /** Ground height for the colliders' heights (default: flat at 0). */
  void setGround(std::function<float(glm::vec2)> ground);
  /**
   * A lane graph by name (null: removed), the game's own (its roads, its
   * aisles): long trips along it plan with vehicle::planLaneTrip(). Replaced
   * whole when the game's lanes change; plans holding the old one finish on it.
   */
  void setLaneGraph(const std::string& name,
                    std::shared_ptr<const nav::LaneGraph> graph);
  /** Drive a path the game made (a patrol route), claimed like any other. */
  void setPath(const Entity& agent, std::shared_ptr<const nav::AgentPath> path);
  /** Its game is planning its next path: never a static obstacle meanwhile
   * (nav::Fleet::setPlanning()). */
  void setPlanning(const Entity& agent, bool planning);
  /** Right of way per agent (none: the component's priority). */
  void setPriorityProvider(std::function<int(const Entity&)> priority);
  /**
   * A vehicle agent's rig, asked each update: the mass it moves besides its
   * own (what it tows and carries), the grip under it (tyres x surface) and
   * its measured wheelbase (nav::vehicle::Rig); its effort is always its
   * component's. The game's own planners derive their spec from the same
   * function (nav::vehicle::applySpec), so planner and driver are one spec.
   * None: no load, grip 0.6 (firm, dry ground).
   */
  void setRigProvider(std::function<nav::vehicle::Rig(const Entity&)> rig);
  /** Plan in this instead of the physics' colliders (no PhysicsSystem3D). */
  void setSpace(std::shared_ptr<const nav::Space> space);
  std::vector<ModuleSetting> settings() override;
  /**
   * The same, shared: for a game's job handles that may outlive this system
   * (cancelled on the scheduler they were given, never a dangling one).
   */
  std::shared_ptr<nav::PlanScheduler> sharedScheduler() const
  {
    return m_scheduler;
  }
  nav::Fleet::State state(const Entity& agent) const;
  /**
   * A driver takes the wheel (a player): the agent's goal or path dropped,
   * the system moves it each update by the controls last given, by its
   * kind's own motion from its spec (nav::Fleet::takeWheel; a vehicle:
   * nav::vehicle::HandDrive), no faster than its speed cap, and writes its
   * transform (and its trailer's) as for any agent, till handBack() stands it
   * where it is. Solid to the other agents meanwhile. False: not an agent,
   * or its kind can't be driven by hand.
   */
  bool takeWheel(const Entity& agent);

protected:
  void create() override;
  void onEntityRemoved(const Entity& entity) override;
  void update(double deltaTime) override;

private:
  // An agent as it stands (a view of its NavAgentComponent, made when it's
  // looked at, never kept): whether it's in the fleet.
  struct Tracked
  {
    Entity entity;
    glm::vec3& axle;
    std::optional<NavHitch>& hitch;
    uint32_t& layers;
    std::string& complaint;
    bool inFleet = false;
  };
  struct Params;
  struct TrackedEntry
  {
    Entity::EntityId id;
    std::optional<Tracked> t;
  };

  std::shared_ptr<const nav::CostLayer> combinedCost() const;
  void draw();
  // Calls fn(id, Tracked&) for every agent, from a list of them made first
  // (so fn may change the fleet), in a buffer kept between calls.
  template <typename TFn>
  void forEachTracked(TFn&& fn) const;
  // What it plans by besides its boxes (none: it can't be an agent, an
  // error logged once).
  std::optional<Params> paramsOf(Tracked& agent);
  bool partOfAgent(const Entity& entity) const;
  // `params` with its boxes (and its trailer's), from its colliders.
  nav::AgentSpec specOf(const Tracked& agent,
                        const nav::AgentSpec& params) const;
  void syncAgents();
  // Every agent's speed cap, from its component to the fleet (what an order
  // that plans reads, written by the game since the last update).
  void syncSpeedCaps();
  void syncWorld();
  std::optional<Tracked> trackedById(Entity::EntityId id) const;
  // An agent (none: not one), and every agent, with its id.
  std::optional<Tracked> trackedOf(const Entity& agent) const;
  void writeBack();

  std::shared_ptr<nav::PlanScheduler> m_scheduler; // (outlives m_fleet)
  std::unique_ptr<nav::Fleet> m_fleet;
  std::map<std::string, std::shared_ptr<const nav::CostLayer>> m_costs;
  std::map<std::string, std::shared_ptr<const nav::LaneGraph>> m_laneGraphs;
  std::function<int(const Entity&)> m_priority;
  std::function<nav::vehicle::Rig(const Entity&)> m_rig;
  std::function<float(glm::vec2)> m_ground;
  std::shared_ptr<const nav::Space> m_givenSpace;
  std::shared_ptr<const nav::Space> m_space;
  // The coarse graph being built (a scheduler job) and what for; the game's
  // edges, and whether the graph has yet to take them.
  nav::PlanScheduler::JobId m_graphJob = 0;
  std::shared_ptr<const nav::Space> m_graphFor;
  std::vector<nav::CoarseGraph::Edge> m_coarseEdges;
  bool m_edgesDirty = false;
  uint64_t m_revision = ~0ull;
  bool m_worldDirty = true;
  bool m_drawPaths = false, m_drawClaims = false, m_drawBlocked = true;
  // forEachTracked()'s list, reused; and whether a walk over it is under way
  // (one inside it, from a game's callback, makes its own).
  mutable std::vector<TrackedEntry> m_tracked;
  mutable bool m_walkingTracked = false;
};

} // namespace sfs
