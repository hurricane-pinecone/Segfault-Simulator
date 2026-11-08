#pragma once

#include "engine/runtime/navigation/agentKind.h"
#include "engine/runtime/navigation/vehicle/planner.h"
#include "engine/runtime/navigation/vehicle/vehicleModel.h"
#include "engine/runtime/navigation/vehicle/vehiclePath.h"

#include <memory>

namespace sfs
{
struct VehicleSpec;
}

namespace sfs::nav::vehicle
{

/**
 * What a vehicle's motion depends on besides its spec: the mass it moves
 * besides its own (towed and carried, kg), the grip under it (friction
 * coefficient: tyres x surface), its driving style (NavAgentComponent::
 * effort), and its wheelbase and fixed (rear) axle as measured from its
 * model (m; the axle ahead of the entity's origin along +X, below 0 behind
 * it), each used where the spec's is 0 ("measure it"). The axle is the
 * vehicle's reference point: what its poses, paths and boxes are of. One game
 * function makes it for both the game's planners and NavigationSystem
 * (setRigProvider): one AgentSpec, one reference point.
 */
struct Rig
{
  float extraMass = 0.0f;
  float grip = 0.6f;
  float effort = 0.25f;
  float wheelbase = 0.0f;
  float axle = 0.0f;
};

/**
 * Fill an agent's motion from a spec's properties and its rig, the outcomes
 * derived, never stored: tightest turn from wheelbase and steer angle;
 * speeding up from power over the whole rig's mass (2P / (m v) to road
 * speed), braking from brake force over it, both no more than the grip on
 * the machine's own weight gives; cornering from grip x g; each times the
 * effort. Towing or loading makes it slower to speed up and stop.
 */
void applySpec(AgentSpec& agent, const VehicleSpec& spec, const Rig& rig);

/** A vehicle's planned way as an AgentPath: the path itself beside its
 * samples. */
class VehicleAgentPath : public AgentPath
{
public:
  explicit VehicleAgentPath(VehiclePath path);
  VehiclePath path;
};

/** The vehicle a spec describes (its boxes, turn, speeds, trailer, modes). */
VehicleModel modelOf(const AgentSpec& agent);
/** The planner's request for a spec and an ask (budgets, goals, world). */
PlanRequest requestOf(const AgentSpec& agent, const PlanAsk& ask);
/** `path` from `fromS` metres on: re-based to start there (s and time 0). */
VehiclePath tail(const VehiclePath& path, float fromS);

/**
 * The "vehicle" agent kind: planNow() (the request's search, hybrid A*
 * by default; smoother; speed profile) and the Follower that drives the
 * path exactly.
 */
class VehicleKind : public AgentKind
{
public:
  AgentPlan
  plan(const AgentSpec& agent,
       const PlanAsk& ask,
       const std::function<void(const AgentPlan&)>& onPartial) const override;
  std::unique_ptr<AgentMotion>
  makeFollower(const AgentSpec& agent) const override;
  std::vector<Footprint>
  footprint(const AgentSpec& agent, Pose2 pose, float hitch) const override;
  std::optional<Pose2>
  towedPose(const AgentSpec& agent, Pose2 pose, float hitch) const override;
};

/** Adds "vehicle" to AgentKinds (once; NavigationSystem does it). */
void addKind();

} // namespace sfs::nav::vehicle
