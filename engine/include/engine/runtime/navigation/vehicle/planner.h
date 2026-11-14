#pragma once

#include "engine/runtime/navigation/coarseGraph.h"
#include "engine/runtime/navigation/costLayer.h"
#include "engine/runtime/navigation/planScheduler.h"
#include "engine/runtime/navigation/planState.h"
#include "engine/runtime/navigation/space.h"
#include "engine/runtime/navigation/vehicle/vehicleModel.h"
#include "engine/runtime/navigation/vehicle/vehiclePath.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sfs::nav::vehicle
{

class Motions;

/**
 * A pose to get to (and, towing, the trailer's angle there, if it matters).
 * `arriveGear` says how the last stretch into it is driven: 0 either way, 1
 * forward, -1 backing (a parking spot a rig is reversed into: it comes to
 * the goal in reverse, whatever it drove before, over at least the rig's own
 * length: the whole rig comes into its spot that way, not a shuffle back at
 * the end).
 */
struct Goal
{
  Pose2 pose;
  std::optional<float> hitch;
  float hitchTolerance = 0.08f;
  int arriveGear = 0;
};

/** The gear the last stretch of a way is driven in (0: none). */
inline int lastGear(std::span<const Segment> segments)
{
  for (auto it = segments.rbegin(); it != segments.rend(); ++it)
    if (it->length > 0.0f)
      return it->gear;
  return 0;
}

/** How far the last stretch of a way runs in its gear (m). */
inline float lastRun(std::span<const Segment> segments)
{
  const int gear = lastGear(segments);
  float run = 0.0f;
  for (auto it = segments.rbegin(); it != segments.rend(); ++it)
  {
    if (it->length <= 0.0f)
      continue;
    if (it->gear != gear)
      break;
    run += it->length;
  }
  return run;
}

/**
 * Whether a way's last stretch is driven as the goal asks (`run`: the least
 * it must run in that gear, the rig's length).
 */
inline bool
arrivesAsAsked(const Goal& goal, std::span<const Segment> segments, float run)
{
  return goal.arriveGear == 0 || (lastGear(segments) == goal.arriveGear &&
                                  lastRun(segments) >= run - 1e-3f);
}

/**
 * One plan: a vehicle from where it is to any of its goals (or anywhere
 * `goalTest` accepts: "anywhere off this other vehicle's way"), over a Space
 * and a CostLayer. How long it may search is a budget of NODES, never the
 * clock, so how busy the machine is never changes the answer.
 */
struct PlanRequest
{
  VehicleModel vehicle;
  Tunables tunables;
  std::string search = "hybridAStar"; // the path search (Searches), by name
  int mode = 0; // the vehicle's mode it's driven in (0: travel)
  std::shared_ptr<const Space> space;
  std::shared_ptr<const CostLayer> cost; // null: ordinary ground
  Pose2 from;
  float hitch = 0.0f;
  std::vector<Goal> goals;
  std::function<bool(Pose2)> goalTest; // (a goal region, besides `goals`)
  /**
   * A corridor to plan along (CoarseGraph::route() for a long trip): the
   * search keeps within `corridorWidth` of the polyline; not finding a way
   * in it is no proof, the search goes on without it.
   */
  std::vector<glm::vec2> corridor;
  float corridorWidth = 8.0f;
  /**
   * The coarse graph (NavigationSystem's): a trip longer than
   * `corridorFrom` with no corridor given takes its corridor from it.
   */
  std::shared_ptr<const CoarseGraph> graph;
  float corridorFrom = 80.0f;
  uint32_t nodeBudget = 20000;   // the first search's
  uint32_t budgetCap = 1u << 20; // doubled up to this
  float margin = 25.0f;          // m round start and goals first searched
  float speedCap = std::numeric_limits<float>::infinity();
  bool smooth = true;
  const std::atomic<bool>* cancel = nullptr;
};

struct PlanResult
{
  PlanState state = PlanState::Pending;
  VehiclePath path; // sampled, its speed profile laid
  float seconds = 0.0f;
  int goal = -1;
  std::vector<Space::Hit> blockers; // Blocked: what hems it in
  PlanStats stats;
  std::string why;
};

/**
 * Plan on this thread. Out of budget it calls `onPartial` with the clear way
 * so far and goes on with twice the budget, up to budgetCap (then Partial).
 */
PlanResult
planNow(const PlanRequest& request,
        const std::function<void(const PlanResult&)>& onPartial = {});

/**
 * THE stand-here check: what the vehicle standing at `pose` touches in the
 * request's space (its boxes and the gap round them, Tunables::gap; its
 * trailer at `hitch` when given, else free to swing). planNow() starts by it
 * and refuses a goal by it ("something stands where it's to stop"); a
 * game's own "can it stop here" asks this, never a check of its own.
 */
std::optional<Space::Hit>
touches(const PlanRequest& request, Pose2 pose, std::optional<float> hitch);

/**
 * How long a way takes for the vehicle over the cost layer (the planner's
 * price, in seconds: backing up dearer, cusps a gear change each); infinite
 * through anything solid if `check`, or past the trailer's limits.
 */
float priceOf(const PlanRequest& request,
              Pose2 from,
              float hitch,
              const std::vector<Segment>& segments,
              bool check);

/**
 * THE check of a way a game laid itself (a field pass, its own sampled
 * curve, a straight onto a hitch): the planner's final check (its boxes and
 * the gap, `Tunables::exactStep`, the trailer swung on its hitch from
 * `hitch` and kept within its limits), either gear allowed. Getting out of a
 * touch at its start is let off within kEscapeReach, as planNow() lets a way
 * out; with `endIn`, where the end itself touches (backing onto what it's to
 * hitch), its last kEscapeReach metres are let off too. Where it first
 * touches, in metres along it (driven either way); nothing: it's clear.
 */
std::optional<float> touchesAlong(const PlanRequest& request,
                                  Pose2 from,
                                  float hitch,
                                  std::span<const Segment> segments,
                                  bool endIn = false);
/**
 * The same with a check made once and kept (its probes prepared once: a
 * live look-ahead's, every frame), stepping `step` metres (the motion between
 * two steps swept, as the search's check: nothing slips between). `rig` is
 * made from a request whose vehicle may reverse (the way's the game's).
 */
std::optional<float> touchesAlong(const Motions& rig,
                                  Pose2 from,
                                  float hitch,
                                  std::span<const Segment> segments,
                                  bool endIn,
                                  float step);

/**
 * Plan on the planning scheduler (nav::PlanScheduler): a one-step job whose
 * one leg is the PlanResult planNow() gives (never the clear way so far: no
 * partial leg is published). Its cancel is the job's. `deadline`: when the
 * vehicle needs it (now standing, else when it runs out of path).
 */
PlanScheduler::JobId submitPlan(PlanScheduler& scheduler,
                                uint64_t owner,
                                std::string label,
                                PlanScheduler::Clock::time_point deadline,
                                PlanRequest request,
                                bool lookAhead = false);

} // namespace sfs::nav::vehicle
