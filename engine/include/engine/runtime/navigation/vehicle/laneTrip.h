#pragma once

#include "engine/runtime/navigation/laneGraph.h"
#include "engine/runtime/navigation/vehicle/planner.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sfs::nav::vehicle
{

/**
 * A long trip along a LaneGraph, the textbook split: the graph search finds
 * the lanes to take (LaneGraph::route(), milliseconds); along each stretch
 * the vehicle drives the lane's own line, no search; the vehicle planner
 * (hybrid A*) searches only the short windows between: getting on (from
 * where it stands to the first stretch), round each corner from one stretch
 * to the next (forward), and getting off to the goal. Every window is a plain
 * planNow() of the trip's own request (the same vehicle, Space and CostLayer:
 * one model).
 *
 * Each stretch is checked whole, as it's driven, by the planner's own check
 * of a laid way (touchesAlong(): the boxes, THE gap, the trailer swung on its
 * hitch from its angle where the stretch starts). Where it touches something
 * (a tree the lane line passes too near, a trailer cutting in on a bend) the
 * trip leaves the lane there: the stretch ends short of it and a window
 * (Window::Round, forward) searches round it, back onto the lane past it,
 * the textbook local repair of a global route. The lane graph stays the
 * game's plain geometry, whatever stands beside it and whatever rig drives
 * it; nothing that touches is ever handed back, so planning again never
 * returns the same blocked way.
 */
struct LaneTripRequest
{
  /** The vehicle, its Space and CostLayer, where it is and its goal. */
  PlanRequest plan;
  LaneGraph::Options lanes;
  bool reverseOn = false;  // getting on may back up (corners never do)
  bool reverseOff = false; // getting off may (backing into a spot)
  // A window's search, once a way's found, looks this many more poses for a
  // quicker one at most (Tunables::improveNodes; the windows are short joins,
  // their ground cheap along the lanes, so the heuristic prunes little).
  uint32_t windowImprove = 400;
  enum class Window
  {
    On,
    Corner,
    Off,
    Round, // off the lane round what's too near it, back on past it
  };
  /**
   * Each window's search (default planNow()): a game may wrap it (its own
   * logging, a retry). Given the window's request and which window it is.
   */
  std::function<PlanResult(const PlanRequest&, Window)> search;
};

struct LaneTrip
{
  /** A part of the trip, in order: a searched window or a lane stretch. */
  struct Part
  {
    std::optional<LaneTripRequest::Window> window; // none: a lane stretch
    PlanResult found;                              // a window's
    Pose2 to;                                      // where the window went
    LaneGraph::Stretch stretch;                    // a lane stretch's
    float hitch = 0.0f; // its trailer's angle where the part ends
  };
  std::vector<Part> parts;
  /**
   * Found: every part's in. Otherwise the state of the window that wasn't
   * found (the parts before it are good: driven up to, the rest planned
   * again from there).
   */
  PlanState state = PlanState::Pending;
  int stretches = 0;     // lane stretches
  int rounds = 0;        // windows round what's too near a lane
  float laneMetres = 0;  // m driven along lanes
  double graphMs = 0.0;  // the graph search's time
  double windowMs = 0.0; // the windows' searches' time
};

/**
 * The trip, or nothing when the lanes aren't worth taking (or there are
 * none near): then plan it as any other way.
 */
std::optional<LaneTrip> planLaneTrip(const LaneGraph& graph,
                                     const LaneTripRequest& request);

} // namespace sfs::nav::vehicle
