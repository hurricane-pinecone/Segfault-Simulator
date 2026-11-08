#pragma once

#include "engine/runtime/navigation/costLayer.h"
#include "engine/runtime/navigation/geometry.h"
#include "engine/runtime/navigation/planState.h"
#include "engine/runtime/navigation/space.h"
#include "engine/runtime/navigation/traffic.h"

#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfs::nav
{

class CoarseGraph;

/**
 * What an agent plans in: what's solid (Space), what ground costs
 * (CostLayer, null: ordinary ground everywhere) and the coarse graph long
 * trips take their corridor from (null: none). Immutable while plans use it:
 * a change makes a new World.
 */
struct World
{
  std::shared_ptr<const Space> space;
  std::shared_ptr<const CostLayer> cost;
  std::shared_ptr<const CoarseGraph> graph;
};

/** Where an agent is sent. */
struct AgentGoal
{
  Pose2 pose;
  std::optional<float> hitch; // a trailer's angle there, if it matters
  int mode = 0;               // the mode it's driven in (0: travel)
  float speedCap = std::numeric_limits<float>::infinity();
  // How it's come to (a vehicle's last stretch): 0 either way, 1 forward, -1
  // backing (a parking spot it's reversed into).
  int arriveGear = 0;
};

/**
 * An agent as its kind plans it: its boxes (from its colliders, in its
 * reference point's frame) and how it moves. Every kind reads what it needs
 * and ignores the rest (a walker has no wheelbase).
 */
struct AgentSpec
{
  std::string kind = "vehicle";
  std::string search = "hybridAStar"; // the path search, by name
  std::vector<Box3> boxes;
  float minRadius = 5.0f, wheelbase = 2.6f, steerRate = 1.2f;
  float topSpeed = 7.0f, reverseSpeed = 2.0f;
  float accel = 1.2f, brake = 2.0f, lateralAccel = 0.9f;
  // (Route cost: its time. A direction change is priced at its real time:
  // VehicleModel::cuspSeconds(), from shuttleSeconds and the derived
  // accel/brake the follower drives with; backing up at reverseSpeed.)
  float shuttleSeconds = 0.5f;
  bool mayReverse = true;
  /** A towed thing: its hitch behind the reference point, its axle behind
   * that, its box in its axle's frame, its angle limits. */
  struct Towed
  {
    float hitchBehind = 1.0f, drawbar = 4.0f;
    Box3 box;
    float forwardLimit = 1.45f, reverseLimit = 0.61f;
  };
  std::optional<Towed> trailer;
  /** Working modes 1.. (an implement down): their boxes and tightest turn. */
  struct Mode
  {
    std::vector<Box3> boxes;
    float minRadius = 5.0f;
    bool forwardOnly = true;
  };
  std::vector<Mode> modes;
};

/**
 * A planned way, any kind's: samples every few centimetres along it (what
 * Traffic claims and the debug view draws), the kind's own form in a
 * derived class.
 */
class AgentPath
{
public:
  struct Sample
  {
    float s = 0.0f; // metres along
    Pose2 pose;
    float hitch = 0.0f;
    float time = 0.0f; // s from its start, by its speed profile
    int gear = 1;      // -1 backing up
  };
  virtual ~AgentPath() = default;
  std::vector<Sample> samples;
  float length() const { return samples.empty() ? 0.0f : samples.back().s; }
  float duration() const
  {
    return samples.empty() ? 0.0f : samples.back().time;
  }
};

/** What a plan is asked. */
struct PlanAsk
{
  World world;
  Pose2 from;
  float hitch = 0.0f;
  std::vector<AgentGoal> goals;
  std::function<bool(Pose2)> goalTest; // a goal region, besides `goals`
  int mode = 0;
  uint32_t nodeBudget = 20000, budgetCap = 1u << 20;
  const std::atomic<bool>* cancel = nullptr;
};

/** What a plan answers (PlanState's meanings: Blocked only when proven). */
struct AgentPlan
{
  PlanState state = PlanState::Pending;
  std::shared_ptr<const AgentPath> path;
  float seconds = 0.0f;
  int goal = -1;
  std::vector<Space::Hit> blockers;
  PlanStats stats;
  std::string why;
};

/**
 * A driver's controls (a player at the wheel), each frame: throttle and
 * brake 0..1, steering -1..1 of full lock (+ left), the gear it's driven in
 * (1 forward, -1 backing up). How they move it is the agent kind's own
 * motion, from the same spec its paths are driven by.
 */
struct Controls
{
  float throttle = 0.0f;
  float brake = 0.0f;
  float steer = 0.0f;
  int gear = 1;
};

/** One agent driving its path (main thread). */
class AgentMotion
{
public:
  virtual ~AgentMotion() = default;
  /**
   * Follow `path` from `fromS` metres along it (0: its start), at `speed`
   * already (m/s).
   */
  virtual void setPath(std::shared_ptr<const AgentPath> path,
                       float speed,
                       float fromS = 0.0f) = 0;
  /** The path it follows (null: none). */
  virtual const AgentPath* path() const = 0;
  struct Step
  {
    bool done = false;    // at its end
    bool waiting = false; // standing to change gear
    // Stood at once from this speed (m/s) at a stop named nearer than its
    // brakes reach (a hard stop nothing priced); 0: none.
    float hardStopFrom = 0.0f;
  };
  /** On by `deltaTime`, no faster than `speedCap`, standing by `stopAt` m. */
  virtual Step step(float deltaTime, float speedCap, float stopAt) = 0;
  virtual float s() const = 0;
  virtual float speed() const = 0; // signed: below 0 backing up
  /**
   * m it goes on along its path before it can stand, braking now (its
   * kind's own stopping distance; a frame of `deltaTime` at its speed
   * first). 0: it stands at once.
   */
  virtual float stoppingDistance(float deltaTime) const
  {
    (void)deltaTime;
    return 0.0f;
  }
  virtual float curvature() const = 0;
  virtual Pose2 pose() const = 0;
  virtual float hitch() const = 0;
  /** The index of its path's sample at or before s(). */
  virtual std::size_t index() const = 0;
  /** Its spec changed (a load, the grip): how it moves now. */
  virtual void setSpec(const AgentSpec&) {}
  /**
   * Driven by hand from `pose` (no path; its path dropped): each step()
   * after this moves it by the last controls given (setControls). False: its
   * kind can't be driven by hand. handBack() ends it, standing where it is.
   */
  virtual bool takeWheel(Pose2 pose, float hitch, float speed)
  {
    (void)pose;
    (void)hitch;
    (void)speed;
    return false;
  }
  virtual void setControls(const Controls&) {}
  virtual void handBack() {}
  virtual bool handDriven() const { return false; }
};

/**
 * An agent kind ("vehicle"; "walker" later): how agents of it plan, move
 * and take up room. One object serves every agent of the kind; plan() is
 * called from worker threads at once.
 */
class AgentKind
{
public:
  virtual ~AgentKind() = default;
  /**
   * Plan (worker-safe). Out of budget it calls `onPartial` with the clear
   * way so far and searches on, as its search's contract says.
   */
  virtual AgentPlan
  plan(const AgentSpec& agent,
       const PlanAsk& ask,
       const std::function<void(const AgentPlan&)>& onPartial) const = 0;
  virtual std::unique_ptr<AgentMotion>
  makeFollower(const AgentSpec& agent) const = 0;
  /** Its boxes on the ground at a pose (its trailer's too). */
  virtual std::vector<Footprint>
  footprint(const AgentSpec& agent, Pose2 pose, float hitch) const = 0;
  /** Where its towed thing's axle is (none: it tows nothing). */
  virtual std::optional<Pose2> towedPose(const AgentSpec&, Pose2, float) const
  {
    return std::nullopt;
  }
};

/**
 * The agent kinds by name: NavigationSystem adds "vehicle"; a game or a
 * later kind adds its own (add("walker", ...)) without touching the system.
 */
class AgentKinds
{
public:
  static void add(std::string name, std::shared_ptr<const AgentKind> kind);
  static std::shared_ptr<const AgentKind> find(std::string_view name);
  static std::vector<std::string> names();
};

} // namespace sfs::nav
