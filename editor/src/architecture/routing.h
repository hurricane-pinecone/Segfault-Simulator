#pragma once

#include <imgui.h>
#include <imgui_internal.h> // ImRect

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

// How the Architecture board's arrows find their way between the boxes: along
// the empty channels round them, never under one.
//
// The channels are lines through the emptiest stretches of the board: the
// gutters between columns of boxes and the gaps between rows, found from the
// boxes themselves, so the whole board's grid, a neighbourhood's columns and a
// box dragged off the grid all work alike (a box that sits on a channel gets
// channels of its own round it, and the channel is blocked where it is). A
// link leaves its box from a side, runs along the gutter beside it, along a
// row gap across the columns between, down the gutter beside its target and
// into the target's side: a shortest path over the channels that pays for
// each bend. Links from one box, or into one box, are cheaper along the
// channels the others already take, so they travel as one trunk and branch
// only where they must.
//
// Each link's route is kept two ways: bundled, every link on a channel along
// its middle (the trunks, for a far view), and spread, each link in a lane of
// its own (a few units apart, a box's links side by side so they read as a
// stream). Drawing moves smoothly between them by one number, so nothing is
// routed again as the view zooms.
//
// Memory: a Router keeps its scratch (the channel grid, the search's arrays,
// the lane bookkeeping) between runs and only grows it; Routes is flat arrays,
// refilled in place. A run costs a shortest-path search per link over the
// channel grid (a few hundred crossings for a board of a few hundred boxes),
// and is made when the boxes or the links change, never per frame.
namespace architecture
{

/** A link to route, between two boxes. */
struct RouteLink
{
  /** The box it leaves: an index into the boxes routed. */
  uint32_t from = 0;
  /** The box it enters: an index into the boxes routed. */
  uint32_t to = 0;
};

/** A stretch of channel some links share, drawn once while they're bundled. */
struct RoutePiece
{
  /** One end, in canvas units. */
  ImVec2 a{0.0f, 0.0f};
  /** The other end, in canvas units. */
  ImVec2 b{0.0f, 0.0f};
  /** The first of its links in Routes::pieceLinks. */
  uint32_t firstLink = 0;
  /** How many links run along it. */
  uint32_t linkCount = 0;
  /** It ends at a box's side, links entering there: `b` is that end. */
  bool entersBox = false;
};

/** Every link's route, in flat arrays. */
struct Routes
{
  /** Each link's corners: from cornerStart[link] to cornerStart[link + 1]. */
  std::vector<uint32_t> cornerStart;
  /** Each corner with the links bundled, in canvas units. */
  std::vector<ImVec2> corners;
  /** How far each corner moves when the links are spread into lanes. */
  std::vector<ImVec2> spread;
  /** Each link's bounds, bundled or spread or anywhere between. */
  std::vector<ImRect> bounds;
  /** Whether each link found a clear way (1); if not (0), it's straight. */
  std::vector<uint8_t> routed;
  /** The stretches of channel the links run along, bundled. */
  std::vector<RoutePiece> pieces;
  /** The links along each piece, piece after piece (indices into links). */
  std::vector<uint32_t> pieceLinks;
};

/** How links keep clear of boxes and of each other (canvas units). */
struct RouteSettings
{
  /** What a bend costs, as a length. */
  float bendCost = 60.0f;
  /** The space kept between a link and any box. */
  float clearance = 12.0f;
  /** The space between spread links, when the channel has room. */
  float laneGap = 6.0f;
  /** The space kept free at a box side's top and bottom by its links. */
  float portMargin = 6.0f;
  /**
   * A channel's cost, times this, for a link whose box (either end) has
   * another link along it already: under 1 bundles them.
   */
  float shareFactor = 0.4f;
  /**
   * A channel's cost grows by this much of itself for each link past the
   * lanes it has room for, so crowded ones are shared out.
   */
  float crowdCost = 0.05f;
};

/**
 * Routes links between boxes. It keeps its scratch between runs: make one
 * and reuse it.
 */
class Router
{
public:
  /** A channel line, and the half-width round it that's free for lanes. */
  struct Line
  {
    /** Where it runs: its x, or its y. */
    float at = 0.0f;
    /** How far its lanes may spread either side of it. */
    float room = 0.0f;
  };

  /**
   * Route every link round the boxes. Deterministic: the same boxes and
   * links give the same routes.
   *
   * @param boxes the boxes, in canvas units
   * @param links the links, between boxes by index
   * @param out set to the routes, its arrays reused
   */
  void route(std::span<const ImRect> boxes,
             std::span<const RouteLink> links,
             Routes& out);

  /** How links keep clear of boxes and each other. */
  RouteSettings& settings() { return m_settings; }

private:
  // A stretch across the board that the same number of boxes cover.
  struct Run
  {
    float lo = 0.0f;
    float hi = 0.0f;
    int cover = 0;
  };

  // Where a box's side meets its gutter: the gutter's line, the stub across
  // to it, and the row lines above and below the port, if the way to them is
  // clear (-1 if not).
  struct Exit
  {
    int line = -1;
    float stub = 0.0f;
    int up = -1;
    int down = -1;
  };

  // A run of a link's route along one channel or one port stub, before lanes.
  struct Leg
  {
    uint32_t link = 0;
    int channel = -1; // -1: a stub at a box's side
    uint32_t group = 0;
    float lo = 0.0f;
    float hi = 0.0f;
    // Which way its link turns off at its ends: below 0 to the left (or
    // up), above 0 to the right (or down). Lanes are ordered by it, its
    // group's average first, so fewer links cross where they turn.
    float bias = 0.0f;
    float groupBias = 0.0f;
    float offset = 0.0f;
  };

  // A point of a route on the channel grid: its x line and y line, -1 where
  // it's a box's side or a port's height.
  struct Point
  {
    ImVec2 at{0.0f, 0.0f};
    int xLine = -1;
    int yLine = -1;
  };

  // Each link's route as found, then its legs and their lanes.
  // A link's use of a stretch of channel or a box side's port, for sorting
  // the uses into pieces and port slots.
  struct Use
  {
    int32_t key[4] = {0, 0, 0, 0};
    float order = 0.0f;
    float order2 = 0.0f;
    uint32_t link = 0;
    uint32_t index = 0;
    bool enters = false;
  };

  void assignLanes();
  void assignPorts(std::span<const ImRect> boxes,
                   std::span<const RouteLink> links,
                   Routes& out);
  void buildChannels(std::span<const ImRect> boxes);
  void buildPieces(Routes& out);
  bool clearAlong(int line, float y0, float y1) const;
  void findExits(std::span<const ImRect> boxes);
  void lines(std::span<const ImRect> boxes, bool vertical);
  void markShared(uint32_t from, uint32_t to);
  void routeOne(std::span<const ImRect> boxes, const RouteLink& link);
  float
  segmentCost(int segment, float length, uint32_t from, uint32_t to) const;

  RouteSettings m_settings;

  // The channel grid: x lines, y lines, and which stretches between their
  // crossings are clear, by segment (vertical ones first, then horizontal,
  // then each box exit's stub and partial run: up, then down).
  std::vector<Line> m_xs;
  std::vector<Line> m_ys;
  // Per line, the stretches of it boxes block: along x lines, from y to y;
  // along y lines, from x to x.
  std::vector<std::vector<std::pair<float, float>>> m_blockedX;
  std::vector<std::vector<std::pair<float, float>>> m_blockedY;
  std::vector<uint8_t> m_clear;
  std::vector<Exit> m_exits; // per box: left, then right
  int m_exitSegments = 0;

  // Which boxes' links already run along each segment: a bit per box; and
  // how many links run along it, against the lanes it has room for.
  std::vector<uint64_t> m_shared;
  std::vector<uint16_t> m_load;
  std::vector<float> m_capacity;
  std::size_t m_words = 0;

  // The search, reused.
  std::vector<float> m_cost;
  std::vector<int> m_parent;
  std::vector<uint32_t> m_seen;
  uint32_t m_stamp = 0;
  std::vector<std::pair<float, int>> m_heap;
  std::vector<Run> m_runs;
  std::vector<Point> m_points;
  std::vector<Point> m_kept; // one route's corners
  std::vector<std::pair<uint32_t, uint32_t>> m_pointRange; // per link
  std::vector<Leg> m_legs;
  std::vector<uint32_t> m_legStart; // per link, one more for the end
  std::vector<int> m_lanes;         // per leg
  std::vector<uint32_t> m_order;
  std::vector<float> m_laneEnds;
  std::vector<std::pair<int, std::pair<float, float>>> m_taken;
  std::vector<std::pair<float, int>> m_events;
  std::vector<int> m_segments; // one route's, as found
  std::vector<Use> m_uses;
};

/**
 * A link's path as drawn: its corners, `spread` of the way from bundled to
 * spread into lanes.
 *
 * @param routes the routes
 * @param link the link
 * @param spread 0 bundled, 1 spread
 * @param out set to its points, in canvas units, its vector reused
 */
void routePath(const Routes& routes,
               uint32_t link,
               float spread,
               std::vector<ImVec2>& out);

} // namespace architecture
