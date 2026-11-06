#pragma once

#include "engine/runtime/navigation/coarseGraph.h"
#include "engine/runtime/navigation/costLayer.h"
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
#include <string>
#include <vector>

namespace sfs
{
class WorkerPool;
}

namespace sfs::nav::vehicle
{

/** A pose to get to (and, towing, the trailer's angle there, if it matters). */
struct Goal
{
  Pose2 pose;
  std::optional<float> hitch;
  float hitchTolerance = 0.08f;
};

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
 * Plans in the background (a few worker threads): submit() returns at once,
 * poll() hands back the latest (Partial while it searches on, then Found or
 * Blocked). Thread-safe for one caller thread.
 */
class Planner
{
public:
  explicit Planner(unsigned threads = 2);
  ~Planner();
  Planner(const Planner&) = delete;
  Planner& operator=(const Planner&) = delete;

  using Ticket = uint64_t;
  Ticket submit(PlanRequest request);
  /** The latest result (Pending till the first); final ones are kept till
   * taken. */
  PlanResult poll(Ticket ticket);
  /** Forget a ticket (cancelling it if it's still searching). */
  void cancel(Ticket ticket);
  std::size_t busy() const;

private:
  struct Job;
  std::unique_ptr<WorkerPool> m_pool;
  struct Jobs;
  std::shared_ptr<Jobs> m_jobs;
  Ticket m_next = 1;
};

} // namespace sfs::nav::vehicle
