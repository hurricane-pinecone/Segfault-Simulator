#pragma once

#include "engine/runtime/navigation/geometry.h"
#include "engine/runtime/navigation/space.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace sfs::nav
{

/**
 * Metres from each cell's middle to the nearest solid cell's middle (`solid`
 * row-major, `w` x `h` cells `cell` across; 1e6 where there's none): an exact
 * Euclidean distance transform (Felzenszwalb and Huttenlocher). THE one: the
 * coarse graph's clearance and CollisionSpace's broad phase are built by it.
 */
std::vector<float>
distanceField(const std::vector<uint8_t>& solid, int w, int h, float cell);

/**
 * The coarse picture of where agents can go, for long trips: the ground of
 * an area on a grid, each cell knowing how far it is from the nearest solid
 * (its clearance), so the gateways and aisles an agent of a given width fits
 * through are known at a glance. route() lays a corridor (a polyline down
 * the middle of the open ground, through gaps wide enough) that a kind's
 * fine search then keeps to; it's a guide, never proof: a fine search that
 * finds no way along it searches on without it.
 *
 * Built on a worker from the static Space when it changes (NavigationSystem
 * does that); immutable after, so any number of plans may use it at once.
 * Games may add edges of their own (worn tracks, a road) with addEdge()
 * before handing it out: cells along them cost less to route through. As
 * they change (tracks wearing in and fading), withEdges() makes the graph
 * again from this one: its clearance shared, only the cells round the edges
 * that came or went priced again (NavigationSystem::setCoarseEdges()).
 */
class CoarseGraph
{
public:
  struct Options
  {
    float cell = 0.25f;         // m: the grid (gaps are told to about this)
    float preferOpen = 1.0f;    // m: closer to a solid than this costs more
    float openCost = 0.1f;      // how much more, at the solid (a factor)
    float rasterInflate = 0.0f; // m more round solids (besides a cell)
  };

  /** The clearance grid of `area` of `space` (worker-safe, a few ms). */
  static std::shared_ptr<CoarseGraph>
  build(const Space& space, Rect area, Options options);
  static std::shared_ptr<CoarseGraph> build(const Space& space, Rect area)
  {
    return build(space, area, Options());
  }

  /**
   * A corridor from `from` to `to` for an agent `halfWidth` either side of
   * its middle line: the cells it passes keep at least that from every
   * solid (to about a cell). Empty when there's none (or either end lies
   * outside the area): no corridor known, plan without one. `narrows`, if
   * given, gets the tight places on the way (where there's less than
   * `tight` to spare either side): poses in the middle of the gap, facing
   * along it, a few metres apart through a long one: where a fine search
   * should aim to thread it.
   */
  std::vector<glm::vec2> route(glm::vec2 from,
                               glm::vec2 to,
                               float halfWidth,
                               std::vector<Pose2>* narrows = nullptr,
                               float tight = 0.6f) const;

  /**
   * A game's own edge (a worn track): routing along it, within `width` of
   * its middle line (the cells whose middles are), costs `factor` (< 1) a
   * metre; where edges cross, the least.
   */
  struct Edge
  {
    glm::vec2 a{0.0f}, b{0.0f};
    float factor = 0.85f;
    float width = 3.0f;
    bool operator==(const Edge&) const = default;
  };
  void addEdge(glm::vec2 a, glm::vec2 b, float factor, float width = 3.0f);
  /**
   * This graph with `edges` as its game's edges instead of those it has: a
   * new graph sharing this one's clearance grid, only the cells round the
   * edges that came or went priced again. This one's left as it is (plans
   * using it go on). Worker-safe.
   */
  std::shared_ptr<CoarseGraph> withEdges(const std::vector<Edge>& edges) const;
  const std::vector<Edge>& edges() const { return m_edges; }

  /** How far `p` is from the nearest solid (m; 0 inside one). */
  float clearance(glm::vec2 p) const;
  Rect area() const { return m_area; }
  float cell() const { return m_options.cell; }

private:
  int index(glm::vec2 p) const;
  void paint(const Edge& e);
  void findNarrows(const std::vector<int>& cells,
                   float halfWidth,
                   float tight,
                   std::vector<Pose2>& out) const;
  Options m_options;
  Rect m_area;
  int m_w = 0, m_h = 0;
  // (m from the nearest solid, per cell: shared by the graphs withEdges()
  // makes from one another.)
  std::shared_ptr<const std::vector<float>> m_clear;
  std::vector<float> m_factor; // routing cost factor per cell (edges)
  float m_least = 1.0f;        // the least of them
  std::vector<Edge> m_edges;
};

} // namespace sfs::nav
