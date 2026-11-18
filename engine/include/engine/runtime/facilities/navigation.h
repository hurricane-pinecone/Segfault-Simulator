#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/facilityAccess.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/service.h"
#include "engine/core/ecs/system.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/navigation/agentKind.h"
#include "engine/runtime/navigation/coarseGraph.h"
#include "engine/runtime/navigation/costLayer.h"
#include "engine/runtime/navigation/fleet.h"
#include "engine/runtime/navigation/geometry.h"
#include "engine/runtime/navigation/laneGraph.h"
#include "engine/runtime/navigation/planScheduler.h"
#include "engine/runtime/navigation/space.h"
#include "engine/runtime/navigation/traffic.h"
#include "engine/runtime/navigation/vehicle/vehicleKind.h"
#include "engine/runtime/sceneManager/scene.h"

#include <glm/glm/ext/vector_float2.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sfs
{

class NavigationSystem;

namespace facility
{

/**
 * The scene's navigation, as game code uses it (`navigation()` on a System,
 * Service, GameObject or Scene): orders for its agents (NavAgentComponent),
 * what they're doing, and the world they plan in. Orders take effect at once
 * and read back in the same step; how they went comes as nav::AgentEvent
 * news. What the engine keeps on the agent's component is written there, not
 * called: NavAgentComponent::hitch, speedCap and active.
 *
 * A value handle over the scene's NavigationSystem, valid for the step that
 * got it: get it where it's used, keep it in no member, and hand workers
 * what it gives out to share (scheduler(), coarseGraph(), laneGraph()).
 * Answers that point into the engine (path(), spec()) are valid for this step
 * only. In a scene without navigation its commands do nothing and its
 * queries give empty answers, with one warning a run (available() tells).
 */
class Navigation
{
public:
  /**
   * An agent's look-ahead plan (planAhead()), once it's finished; taken by
   * the call.
   * @param agent the agent's entity
   * @return the plan; none while it's on the way, or with none asked
   */
  std::optional<nav::AgentPlan> ahead(const Entity& agent) const;

  /**
   * How far along its path an agent is.
   * @param agent the agent's entity
   * @return metres along; 0 with no path
   */
  float along(const Entity& agent) const;

  /** @return whether the scene has navigation behind this facade */
  inline bool available() const { return m_navigation != nullptr; }

  /**
   * Stops an agent and forgets its goal.
   * @param agent the agent's entity
   */
  void cancel(const Entity& agent) const;

  /**
   * The game's own edges on the coarse graph (setCoarseEdges()).
   * @return the edges; empty without navigation
   */
  const std::vector<nav::CoarseGraph::Edge>& coarseEdges() const;

  /**
   * The coarse graph long trips take their corridor from, as built from the
   * scene's solids and the game's edges. It outlives the navigation: hand it
   * to workers.
   * @return the graph; null before the first is built, or without navigation
   */
  std::shared_ptr<const nav::CoarseGraph> coarseGraph() const;

  /**
   * Has a pair keep their own spacing (a follower beside its leader), or stop.
   * @param a one agent's entity
   * @param b the other's
   * @param on whether they cooperate
   */
  void cooperate(const Entity& a, const Entity& b, bool on) const;

  /**
   * What an agent did so far (plans, replans, stops), for diagnostics.
   * @param agent the agent's entity
   * @return its counters; zero for one that isn't an agent
   */
  nav::Fleet::Counters counters(const Entity& agent) const;

  /**
   * How sharply an agent is turning.
   * @param agent the agent's entity
   * @return 1/m, positive to its left; 0 for one that isn't an agent
   */
  float curvature(const Entity& agent) const;

  /**
   * The ground an agent's body covers where it stands.
   * @param agent the agent's entity
   * @return its footprints; empty for one that isn't an agent
   */
  std::vector<nav::Footprint> footprint(const Entity& agent) const;

  /**
   * Stands a hand-driven agent where it is and gives it back to the
   * navigation (takeWheel()).
   * @param agent the agent's entity
   */
  void handBack(const Entity& agent) const;

  /**
   * Whether a driver has the wheel (takeWheel()).
   * @param agent the agent's entity
   * @return true while it's driven by hand
   */
  bool handDriven(const Entity& agent) const;

  /**
   * The angle between an agent and what it tows.
   * @param agent the agent's entity
   * @return radians; 0 with nothing towed
   */
  float hitchAngle(const Entity& agent) const;

  /**
   * The navigation of a registry's scene, as the accessors give it.
   * @param registry the registry (null: none)
   * @return its facade, absent when it has no NavigationSystem
   */
  static Navigation in(Registry* registry);

  /**
   * Whether an entity is one of the navigation's agents now (planned and
   * moved: active, and able to plan).
   * @param agent the entity
   * @return true if it is
   */
  bool isAgent(const Entity& agent) const;

  /**
   * A lane graph the game gave (setLaneGraph()). It outlives the
   * navigation: hand it to workers.
   * @param name its name
   * @return the graph; null for none by that name
   */
  std::shared_ptr<const nav::LaneGraph>
  laneGraph(const std::string& name) const;

  /**
   * The path an agent drives, valid for this step only: compare or read it
   * now, never keep it.
   * @param agent the agent's entity
   * @return its path; null with none
   */
  const nav::AgentPath* path(const Entity& agent) const;

  /**
   * Plans an agent's next way ahead, from where its path ends, needed by
   * when it gets there (ahead() gives it).
   * @param agent the agent's entity
   * @param ask what to plan
   * @return whether it was started
   */
  bool planAhead(const Entity& agent, nav::PlanAsk ask) const;

  /**
   * Plans now, on this thread: only for a check that truly needs its answer
   * this frame; anything else goes through scheduler().
   * @param agent the agent's entity
   * @param ask what to plan
   * @return the plan
   */
  nav::AgentPlan planNow(const Entity& agent, nav::PlanAsk ask) const;

  /**
   * Where an agent's reference point is and which way it faces.
   * @param agent the agent's entity
   * @return its pose; the origin for one that isn't an agent
   */
  nav::Pose2 pose(const Entity& agent) const;

  /**
   * Whether one agent ranks above another in traffic (reserves its way
   * first).
   * @param a one agent's entity
   * @param b the other's
   * @return true if a ranks above b
   */
  bool ranksAbove(const Entity& a, const Entity& b) const;

  /**
   * Reads an agent's colliders and fields again, after changing them.
   * @param agent the agent's entity
   */
  void refresh(const Entity& agent) const;

  /**
   * Sends an agent somewhere: a new request (the last one counts).
   * @param agent the agent's entity
   * @param goal where to
   * @return the request's id; 0 without navigation
   */
  uint64_t requestGoal(const Entity& agent, const nav::AgentGoal& goal) const;

  /**
   * Sends an agent to the nearest of several goals, or anywhere `goalTest`
   * accepts.
   * @param agent the agent's entity
   * @param goals where to
   * @param goalTest what else counts as there (none: only the goals)
   * @return the request's id; 0 without navigation
   */
  uint64_t requestGoals(const Entity& agent,
                        std::vector<nav::AgentGoal> goals,
                        std::function<bool(nav::Pose2)> goalTest = {}) const;

  /**
   * What an agent is to do this frame in traffic: why it stands, where, and
   * for whom.
   * @param agent the agent's entity
   * @return its schedule; on its way for one that isn't an agent
   */
  nav::Traffic::Schedule schedule(const Entity& agent) const;

  /**
   * THE planning scheduler: every path job of the scene (the agents', the
   * coarse graph, a game's own) runs on its one pool. Shared, so a game's job
   * handles may outlive the navigation.
   * @return the scheduler; null without navigation
   */
  std::shared_ptr<nav::PlanScheduler> scheduler() const;

  /**
   * Gives the coarse graph the game's own edges (its worn tracks), replacing
   * the last ones.
   * @param edges the edges
   */
  void setCoarseEdges(std::vector<nav::CoarseGraph::Edge> edges) const;

  /**
   * Steers a hand-driven agent (takeWheel()).
   * @param agent the agent's entity
   * @param controls the driver's controls
   */
  void setControls(const Entity& agent, const nav::Controls& controls) const;

  /**
   * Sets a ground cost by name; all of them multiply.
   * @param name its name
   * @param layer the cost (null: removed)
   */
  void setCostLayer(const std::string& name,
                    std::shared_ptr<const nav::CostLayer> layer) const;

  /**
   * Sets the ground height the colliders' heights are measured from
   * (default: flat at 0).
   * @param ground the height at a point
   */
  void setGround(std::function<float(glm::vec2)> ground) const;

  /**
   * Sets a lane graph by name, the game's own (its roads): long trips along
   * it plan by its lanes. Plans holding the old one finish on it.
   * @param name its name
   * @param graph the graph (null: removed)
   */
  void setLaneGraph(const std::string& name,
                    std::shared_ptr<const nav::LaneGraph> graph) const;

  /**
   * Has an agent drive a path the game made (a patrol route), claimed like
   * any other.
   * @param agent the agent's entity
   * @param path the path
   */
  void setPath(const Entity& agent,
               std::shared_ptr<const nav::AgentPath> path) const;

  /**
   * Says the game is planning an agent's next path, so it's never a static
   * obstacle meanwhile.
   * @param agent the agent's entity
   * @param planning whether it is
   */
  void setPlanning(const Entity& agent, bool planning) const;

  /**
   * Sets who goes first, asked per agent each update (none: the component's
   * priority).
   * @param priority an agent's right of way
   */
  void setPriorityProvider(std::function<int(const Entity&)> priority) const;

  /**
   * Sets a vehicle agent's rig, asked each update: the mass it moves besides
   * its own, the grip under it and its measured wheelbase.
   * @param rig an agent's rig
   */
  void
  setRigProvider(std::function<nav::vehicle::Rig(const Entity&)> rig) const;

  /**
   * Has the agents plan in this space instead of the physics' colliders.
   * @param space the space (null: the colliders again)
   */
  void setSpace(std::shared_ptr<const nav::Space> space) const;

  /** @return the navigation's settings; the defaults without navigation */
  const nav::Fleet::Settings& settings() const;

  /**
   * Whether an agent is a static obstacle in others' plans now. It reads the
   * speed cap the navigation last took from NavAgentComponent (its update,
   * or an order that plans): one written since counts from then.
   * @param agent the agent's entity
   * @return true if it is
   */
  bool solid(const Entity& agent) const;

  /**
   * The spec an agent is planned and driven by, valid for this step only.
   * @param agent the agent's entity
   * @return its spec; null for one that isn't an agent
   */
  const nav::AgentSpec* spec(const Entity& agent) const;

  /**
   * How fast an agent is going.
   * @param agent the agent's entity
   * @return m/s, negative backing up; 0 for one that isn't an agent
   */
  float speed(const Entity& agent) const;

  /**
   * What an agent is doing.
   * @param agent the agent's entity
   * @return its state; Idle for one that isn't an agent
   */
  nav::Fleet::State state(const Entity& agent) const;

  /**
   * A driver takes the wheel (a player): the agent's goal or path dropped,
   * it's moved by the controls last given (setControls()) till handBack().
   * @param agent the agent's entity
   * @return false if it isn't an agent, or its kind can't be driven by hand
   */
  bool takeWheel(const Entity& agent) const;

private:
  inline explicit Navigation(NavigationSystem* navigation)
      : m_navigation(navigation)
  {
  }

  // The navigation; null (and warned about once) when the scene has none.
  NavigationSystem* backend() const;

  // The fleet's id for an agent's entity.
  static nav::Fleet::AgentId idOf(const Entity& agent);

  NavigationSystem* m_navigation = nullptr;
};

} // namespace facility

inline facility::Navigation System::navigation() const
{
  return facility::Navigation::in(registry);
}

inline facility::Navigation Service::navigation() const
{
  return facility::Navigation::in(m_registry);
}

inline facility::Navigation GameObject::navigation() const
{
  return m_scene ? m_scene->navigation() : facility::Navigation::in(nullptr);
}

inline facility::Navigation Scene::navigation()
{
  return facility::Navigation::in(&m_registry);
}

} // namespace sfs
