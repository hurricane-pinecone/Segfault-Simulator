#pragma once

#include "engine/core/components/navAgentComponent.h"
#include "engine/core/ecs/system.h"
#include "engine/runtime/navigation/agentKind.h"
#include "engine/runtime/navigation/coarseGraph.h"
#include "engine/runtime/navigation/costLayer.h"
#include "engine/runtime/navigation/fleet.h"
#include "engine/runtime/navigation/laneGraph.h"
#include "engine/runtime/navigation/vehicle/vehicleKind.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float3.hpp"

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
 * (requestGoal) and hears how it went (events()); it never moves the agent
 * itself meanwhile: the system writes the agent's transform (and its
 * trailer's) each update.
 *
 * What's solid comes from PhysicsSystem3D: a frozen copy of the static
 * colliders (PhysicsSystem3D::snapshot()), rebuilt when its revision
 * changes, with the coarse graph long trips take their corridor from (built
 * on the scheduler). Agents leave themselves out; one with nothing to do (or
 * held by its game) is solid to the others' plans, one moving never is (they
 * keep out of each other's reservations by rank instead: see nav::Fleet and
 * nav::Traffic). A game adds what ground costs (setCostLayer:
 * crop, worn tracks; layers multiply) and who goes first
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
 * Debug views (paths, claims, what blocks a way) are drawn into
 * RenderSystem::debugLines() while debugFeatures::enabled(), switched on in
 * settings(). A scene lists it by name ("NavigationSystem") after
 * "PhysicsSystem3D".
 */
class NavigationSystem : public System
{
public:
  NavigationSystem();
  ~NavigationSystem() override;
  NavigationSystem(const NavigationSystem&) = delete;
  NavigationSystem& operator=(const NavigationSystem&) = delete;

  /** Send an agent somewhere: a new request (the last one counts), its id. */
  uint64_t requestGoal(const Entity& agent, const nav::AgentGoal& goal);
  /** To the nearest of several, or anywhere `goalTest` accepts. */
  uint64_t requestGoals(const Entity& agent,
                        std::vector<nav::AgentGoal> goals,
                        std::function<bool(nav::Pose2)> goalTest = {});
  /** Stop and forget its goal. */
  void cancel(const Entity& agent);
  /**
   * Drive it no faster than this (m/s; 0 stands it, braking): the game's
   * live limit (a working pace, a held pace). Infinity: none.
   */
  void setSpeedCap(const Entity& agent, float cap);
  /** Its game is planning its next path: never a static obstacle meanwhile
   * (nav::Fleet::setPlanning()). */
  void setPlanning(const Entity& agent, bool planning);
  /**
   * Leave an agent out (false) or take it back: one carried by something
   * else, switched off. Out, it isn't planned, moved or claimed and isn't
   * solid to the others.
   */
  void setActive(const Entity& agent, bool active);
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
  void setControls(const Entity& agent, const nav::Controls& controls);
  void handBack(const Entity& agent);
  bool handDriven(const Entity& agent) const;
  /** Drive a path the game made (a field pass), claimed like any other. */
  void setPath(const Entity& agent, std::shared_ptr<const nav::AgentPath> path);
  /**
   * Plan now on this thread: only for a check that truly needs its answer
   * this frame; anything else goes through scheduler().
   */
  nav::AgentPlan planNow(const Entity& agent, nav::PlanAsk ask) const;
  /**
   * THE planning scheduler: every path job of the scene (the agents', the
   * coarse graph, a game's own) runs on its one pool.
   */
  nav::PlanScheduler& scheduler() { return *m_scheduler; }
  /**
   * The same, shared: for a game's job handles that may outlive this system
   * (cancelled on the scheduler they were given, never a dangling one).
   */
  std::shared_ptr<nav::PlanScheduler> sharedScheduler() const
  {
    return m_scheduler;
  }
  /** Plan an agent's next way ahead (nav::Fleet::planAhead()). */
  bool planAhead(const Entity& agent, nav::PlanAsk ask);
  /** Its look-ahead plan once finished (nav::Fleet::ahead()). */
  std::optional<nav::AgentPlan> ahead(const Entity& agent);

  /**
   * A lane graph by name (null: removed), the game's own (its roads, its
   * aisles): long trips along it plan with vehicle::planLaneTrip(). Replaced
   * whole when the game's lanes change; plans holding the old one finish on it.
   */
  void setLaneGraph(const std::string& name,
                    std::shared_ptr<const nav::LaneGraph> graph);
  std::shared_ptr<const nav::LaneGraph>
  laneGraph(const std::string& name) const;

  /**
   * The game's own edges on the coarse graph (its worn tracks: cheaper to
   * route along, so long trips' corridors take them), replacing the last
   * ones: put on the graph as it is and on every one built after, on the
   * scheduler (nav::CoarseGraph::withEdges(): only the cells round what came
   * or went priced again). Plans holding the old graph finish on it.
   */
  void setCoarseEdges(std::vector<nav::CoarseGraph::Edge> edges);
  const std::vector<nav::CoarseGraph::Edge>& coarseEdges() const
  {
    return m_coarseEdges;
  }

  /** A ground cost by name (null: removed); all of them multiply. */
  void setCostLayer(const std::string& name,
                    std::shared_ptr<const nav::CostLayer> layer);
  /** Right of way per agent (none: the component's priority). */
  void setPriorityProvider(std::function<int(const Entity&)> priority);
  /** A pair that keep their own spacing (a cart beside its combine). */
  void cooperate(const Entity& a, const Entity& b, bool on);
  /** A towed thing on an agent's hitch (NavAgentComponent::hitch). */
  using Hitch = NavHitch;
  /**
   * THE measure of a mounted part's box as planned (one function: the
   * agent's boxes here, Traffic's, and a game's own planners alike): the box
   * lo..hi of a collider whose frame is `inAgent` in the agent's own frame
   * (level, its origin the reference point: x along it, heights up from the
   * agent's origin), bounded there, exactly where it's held (its bottom
   * never below 0). Its true size: no tilt is ever baked in (the ground's
   * tilt is put on per checked pose by nav::stanceOf()).
   */
  static nav::Box3 mountedBox(const glm::mat4& inAgent,
                              const glm::vec3& lo,
                              const glm::vec3& hi);
  /** Put on / take off its NavAgentComponent::hitch. */
  void hitch(const Entity& agent, const Hitch& hitch);
  void unhitch(const Entity& agent);
  /**
   * A vehicle agent's rig, asked each update: the mass it moves besides its
   * own (what it tows and carries), the grip under it (tyres x surface) and
   * its measured wheelbase (nav::vehicle::Rig); its effort is always its
   * component's. The game's own planners derive their spec from the same
   * function (nav::vehicle::applySpec), so planner and driver are one spec.
   * None: no load, grip 0.6 (firm, dry ground).
   */
  void setRigProvider(std::function<nav::vehicle::Rig(const Entity&)> rig);
  /** Ground height for the colliders' heights (default: flat at 0). */
  void setGround(std::function<float(glm::vec2)> ground);
  /** Plan in this instead of the physics' colliders (no PhysicsSystem3D). */
  void setSpace(std::shared_ptr<const nav::Space> space);
  /** Read an agent's colliders and fields again (after changing them). */
  void refresh(const Entity& agent);

  nav::Fleet::State state(const Entity& agent) const;
  struct Event
  {
    Entity agent;
    nav::Fleet::Event what;
  };
  /** What happened to agents during the last update. */
  const std::vector<Event>& events() const { return m_events; }
  /** Everything about the agents (paths, claims, counters). */
  const nav::Fleet& fleet() const { return *m_fleet; }

  std::vector<ModuleSetting> settings() override;

protected:
  void update(double deltaTime) override;
  void onEntityRemoved(const Entity& entity) override;

private:
  // An agent as it stands (a view of its NavAgentComponent, made when it's
  // looked at, never kept): whether it's in the fleet, and the plan
  // parameters it has there (or is about to be given).
  struct Tracked
  {
    Entity entity;
    glm::vec3& axle;
    std::optional<Hitch>& hitch;
    uint32_t& layers;
    std::string& complaint;
    bool inFleet = false;
    nav::AgentSpec params;
  };
  // Its NavAgentComponent (null: not one of its agents).
  NavAgentComponent* agentOf(const Entity& agent);
  void syncAgents();
  void syncWorld();
  void writeBack();
  void draw();
  struct Params;
  // What it plans by besides its boxes (none: it can't be an agent, an
  // error logged once).
  std::optional<Params> paramsOf(Tracked& agent);
  // With its boxes (and its trailer's), from its colliders.
  nav::AgentSpec specOf(const Tracked& agent) const;
  bool partOfAgent(const Entity& entity) const;
  std::shared_ptr<const nav::CostLayer> combinedCost() const;

  std::shared_ptr<nav::PlanScheduler> m_scheduler; // (outlives m_fleet)
  std::unique_ptr<nav::Fleet> m_fleet;
  // An agent (none: not one), and every agent, with its id.
  std::optional<Tracked> trackedOf(const Entity& agent) const;
  std::optional<Tracked> trackedById(Entity::EntityId id) const;
  struct TrackedEntry
  {
    Entity::EntityId id;
    std::optional<Tracked> t;
  };
  std::vector<TrackedEntry> tracked() const;
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
  std::vector<Event> m_events;
  bool m_drawPaths = false, m_drawClaims = false, m_drawBlocked = true;
};

} // namespace sfs
