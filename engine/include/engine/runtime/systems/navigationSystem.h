#pragma once

#include "engine/core/ecs/system.h"
#include "engine/runtime/navigation/agentKind.h"
#include "engine/runtime/navigation/costLayer.h"
#include "engine/runtime/navigation/fleet.h"
#include "engine/runtime/navigation/vehicle/vehicleKind.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sfs
{

class WorkerPool;

/**
 * Navigation for AI agents (NavAgentComponent): it plans their ways round
 * the scene's colliders on worker threads, drives them along and settles
 * who gives way when they meet. A game sends an agent somewhere
 * (requestGoal) and hears how it went (events()); it never moves the agent
 * itself meanwhile: the system writes the agent's transform (and its
 * trailer's) each update.
 *
 * What's solid comes from PhysicsSystem3D: a frozen copy of the static
 * colliders (PhysicsSystem3D::snapshot()), rebuilt when its revision
 * changes, with the coarse graph long trips take their corridor from (built
 * on a worker). Agents leave themselves out; one with nothing to do (or held
 * by its game) is solid to the others' plans, one moving never is (they keep
 * out of each other's reservations by rank instead: see nav::Fleet and
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
  /** Plan now on this thread (a game's own checks, on its own worker). */
  nav::AgentPlan planNow(const Entity& agent, nav::PlanAsk ask) const;

  /** A ground cost by name (null: removed); all of them multiply. */
  void setCostLayer(const std::string& name,
                    std::shared_ptr<const nav::CostLayer> layer);
  /** Right of way per agent (none: the component's priority). */
  void setPriorityProvider(std::function<int(const Entity&)> priority);
  /** A pair that keep their own spacing (a cart beside its combine). */
  void cooperate(const Entity& a, const Entity& b, bool on);
  /**
   * A towed thing on an agent's hitch (a trailer, an implement on its
   * drawbar): the game hitches and unhitches, the system then plans and
   * drives the rig and writes the towed entity's transform. Angles are
   * degrees its drawbar may swing going forward / backing up.
   */
  struct Hitch
  {
    Entity towed;
    float hitchBehind = 1.0f; // m, the hitch behind the agent's axle
    float drawbar = 4.0f;     // m, the towed thing's axle behind the hitch
    float forwardLimit = 83.0f, reverseLimit = 35.0f;
    // Mounted (on a linkage or front mount): it moves with the agent, its
    // colliders part of the agent's boxes where it is when hitched, never
    // swinging (the hitch numbers unused), measured by mountedBox().
    bool rigid = false;
    // m a mounted part's bottom is planned lower than it's held standing
    // (driven, the body pitches into ruts and on its springs).
    float rideDrop = 0.0f;
  };
  /**
   * THE measure of a mounted part's box as planned (one function: the
   * agent's boxes here, Traffic's, and a game's own planners alike): the box
   * lo..hi in the frame `world` (a part's world matrix), bounded in the
   * agent's flat frame (its reference point `at`, facing its heading:
   * however the part tilts with a lift, the plan holds the physics' shape),
   * heights above `ground` under the box's middle, its bottom `drop` lower
   * and never below the ground.
   */
  static nav::Box3 mountedBox(const glm::mat4& world,
                              const glm::vec3& lo,
                              const glm::vec3& hi,
                              const nav::Pose2& at,
                              const std::function<float(glm::vec2)>& ground,
                              float drop);
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

private:
  struct Tracked;
  void syncAgents();
  void syncWorld();
  void writeBack();
  void draw();
  struct Params;
  // What it plans by besides its boxes (none: it can't be an agent, an
  // error logged once).
  std::optional<Params> paramsOf(const Entity& agent);
  // With its boxes (and its trailer's), from its colliders.
  nav::AgentSpec specOf(const Tracked& agent) const;
  bool partOfAgent(const Entity& entity) const;
  std::shared_ptr<const nav::CostLayer> combinedCost() const;

  std::unique_ptr<nav::Fleet> m_fleet;
  std::unique_ptr<WorkerPool> m_graphWorker;
  std::map<Entity::EntityId, std::unique_ptr<Tracked>> m_tracked;
  std::map<std::string, std::shared_ptr<const nav::CostLayer>> m_costs;
  std::function<int(const Entity&)> m_priority;
  std::function<nav::vehicle::Rig(const Entity&)> m_rig;
  // What each agent tows (live state, set by the game).
  std::map<Entity::EntityId, Hitch> m_hitches;
  std::map<Entity::EntityId, float> m_speedCaps;
  std::set<Entity::EntityId> m_inactive;
  std::function<float(glm::vec2)> m_ground;
  std::shared_ptr<const nav::Space> m_givenSpace;
  std::shared_ptr<const nav::Space> m_space;
  struct GraphJob;
  std::shared_ptr<GraphJob> m_graphJob;
  uint64_t m_revision = ~0ull;
  bool m_worldDirty = true;
  std::vector<Event> m_events;
  std::map<Entity::EntityId, std::string> m_complaints; // (logged once)
  bool m_drawPaths = false, m_drawClaims = false, m_drawBlocked = true;
};

} // namespace sfs
