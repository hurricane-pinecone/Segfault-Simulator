#pragma once

#include "engine/runtime/navigation/geometry.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace sfs::nav
{

/**
 * A network of lanes to drive along (roads, tracks, aisles: whatever the game
 * says they are). The game decides the graph: its nodes (junctions, ends),
 * its DIRECTED lanes between them, each with the line it's driven along (a
 * lane's centreline, on whichever side of its road the game keeps to), how
 * fast it may be driven and what a metre of it costs. Lanes meet at nodes: a
 * lane ending at a node goes on along any lane starting there.
 *
 * route() is the coarse stage of a long trip: onto the nearest lane going the
 * right way, the cheapest way through the network (A*), off at the lane
 * nearest the goal; handed back as stretches to drive without a stop, each
 * joined to the next (a corner at a junction) and to the trip's ends by a
 * short fine search (vehicle::planLaneTrip()). Immutable once handed to the
 * NavigationSystem (setLaneGraph()): any number of plans use it at once.
 */
class LaneGraph
{
public:
  struct Lane
  {
    int from = -1, to = -1;      // nodes
    std::vector<glm::vec2> line; // driven from `from`'s end to `to`'s
    float speed = 10.0f;         // m/s, the most it's driven at
    float cost = 1.0f;           // per metre (a factor: < 1 preferred)
    uint32_t tag = 0;            // the game's own (which road)
  };

  /** A stretch of lanes to drive without a stop. */
  struct Stretch
  {
    std::vector<Pose2> poses;     // a point every Options::step metres
    std::vector<float> curvature; // 1/m at each, + to the left
    float speed = 10.0f;          // the least of its lanes'
    std::vector<uint32_t> tags;   // its lanes' tags, in order
  };

  struct Options
  {
    float lead = 8.0f;     // m along the first lane before the first stretch
                           // starts (and short of where it leaves the last):
                           // room to turn on and off
    float radius = 6.0f;   // m: a corner's stretches are cut back for this
    float corner = 0.35f;  // rad: a junction turned more than this is a corner
    float reach = 30.0f;   // m: the most a trip's ends may be from a lane
    float worthIt = 0.85f; // getting on and off at least this share of the
                           // way straight there: not worth taking the lanes
    float step = 0.5f;     // m between a stretch's poses
    float least = 4.0f;    // m: a stretch shorter than this (after its cuts)
                           // is left out (the searches either side join it)
  };

  int addNode(glm::vec2 at);
  /** A lane (its line needs two points or more); its index. */
  int addLane(Lane lane);

  const std::vector<glm::vec2>& nodes() const { return m_nodes; }
  const std::vector<Lane>& lanes() const { return m_lanes; }

  /** Where a point is nearest a lane: which, how far along it, how far. */
  struct Spot
  {
    int lane = -1;
    float along = 0.0f;
    float distance = 0.0f;
  };
  /** The nearest lane to `p` within `within` (lane -1: none). */
  Spot nearest(glm::vec2 p, float within) const;

  /**
   * The way along the lanes from `from` to `to` (the cheapest, A*). Nothing
   * if either end is further than Options::reach from every lane, there's no
   * way through, or it isn't worth it (Options::worthIt).
   */
  std::optional<std::vector<Stretch>>
  route(glm::vec2 from, glm::vec2 to, const Options& options) const;

private:
  // Every lane within `slack` of the nearest (the ends a trip may take: both
  // ways along a road).
  std::vector<Spot> near(glm::vec2 p, float reach, float slack) const;
  glm::vec2 pointAt(int lane, float along) const;
  std::vector<glm::vec2> m_nodes;
  std::vector<Lane> m_lanes;
  std::vector<std::vector<float>> m_along; // per lane: metres at each point
  std::vector<std::vector<int>> m_out;     // per node: lanes leaving it
};

} // namespace sfs::nav
