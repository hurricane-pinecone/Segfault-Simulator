#pragma once

#include "engine/runtime/navigation/search.h"
#include "engine/runtime/navigation/vehicle/motions.h"

namespace sfs::nav::vehicle
{

/**
 * A way a search found: its poses (the start first), the cost (seconds) to
 * each, the trailer's angle there, and the segments between them; every
 * joint at zero curvature (a search's motions start and end straight), so
 * the smoother may shortcut between any two.
 */
struct Way
{
  std::vector<Pose2> poses;
  std::vector<float> g;
  std::vector<float> hitch;
  std::vector<std::vector<Segment>> links; // poses[k] -> poses[k + 1]
  int goal = -1;                           // which of the request's goals
};

/** What a search is given: the request's vehicle as Motions, where to look. */
struct SearchContext
{
  const Motions& motions;
  Rect area;                       // the search keeps within it
  std::vector<glm::vec2> corridor; // and within corridorWidth of this
  /**
   * Poses in the middle of the tight gaps on the way (CoarseGraph::route's
   * narrows), facing along them: a search may aim shots at them to thread
   * a gap exactly. A hint only (a search may ignore them).
   */
  std::vector<Pose2> vias;
};

/**
 * A vehicle's path search (SearchBase's contract): it hands back its way as a
 * Way of zero-curvature joints, which the smoother, speed profile and
 * follower take on whatever search found it.
 */
class ISearch : public SearchBase
{
public:
  virtual Way found() const = 0;
  virtual Way bestSoFar() const = 0;
  /** How it got to its way (PlanStats' ways, first and best; diagnostics). */
  virtual void describe(PlanStats&) const {}
};

using Searches = SearchRegistry<SearchContext, ISearch>;

/**
 * The built-in search's heuristic at some poses, for a request (tests and
 * diagnostics): its parts, each in seconds to the request's nearest goal,
 * built over the area its first search covers (round the start and goals,
 * Planner's margin), as a plan of the request builds them. `twoD` (the 2D
 * map round the solids at top speed over the cheapest ground) is a lower
 * bound of the time any way takes; `turning` (cells x 8 directions, turning
 * charged) and `heading` (the heading-aware map, hybrid A* run back from the
 * goals over coarse bins) are better-informed guides that can overstate.
 */
struct HeuristicParts
{
  float twoD = 0.0f, turning = 0.0f, heading = 0.0f;
};
std::vector<HeuristicParts> heuristicAt(const PlanRequest& request,
                                        std::span<const Pose2> poses);
/** The vehicle kind's searches: "hybridAStar" built in. */
Searches& searches();

} // namespace sfs::nav::vehicle
