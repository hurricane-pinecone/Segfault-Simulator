#pragma once

#include "architecture/graph.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// What the Architecture board shows and lights up: which kinds of node and
// edge are on, the boxes and edges round the one being looked at, and the
// nodes a search finds.
namespace architecture
{

/** The kinds of node and edge the board shows (all of them by default). */
struct Filter
{
  /** Whether an edge shows: its kind is on, and so are both its nodes'. */
  bool shows(const Graph& graph, const Edge& edge) const;

  /** Whether a node shows: its kind is on. */
  bool shows(const Node& node) const
  {
    return nodeKinds[static_cast<std::size_t>(node.kind)];
  }

  /** Each edge kind's switch, by EdgeKind. */
  std::array<bool, EDGE_KINDS> edgeKinds = filled<EDGE_KINDS>();
  /** Each node kind's switch, by NodeKind. */
  std::array<bool, NODE_KINDS> nodeKinds = filled<NODE_KINDS>();

private:
  template <std::size_t N>
  static constexpr std::array<bool, N> filled()
  {
    std::array<bool, N> on{};
    on.fill(true);
    return on;
  }
};

/** What's lit while something is looked at; the rest is dimmed. */
struct Lit
{
  /** Something is looked at: what isn't lit is dimmed. */
  bool active = false;
  /** Each edge's: lit (1) or not (0), by index into Graph::edges. */
  std::vector<uint8_t> edges;
  /** Each node's: lit (1) or not (0), by index into Graph::nodes. */
  std::vector<uint8_t> nodes;
};

/** A node's neighbours by the edges a filter shows. */
struct Neighbourhood
{
  /** The node they're round. */
  uint32_t centre = 0;
  /**
   * The nodes pointing at it (whose code touches it), by kind then name; one
   * it points at too is in `outgoing` instead. Each is listed once.
   */
  std::vector<uint32_t> incoming;
  /** The nodes it points at (its code touches), by kind then name. */
  std::vector<uint32_t> outgoing;
};

/** A node a search found. */
struct SearchHit
{
  /** The node: an index into Graph::nodes. */
  uint32_t node = 0;
  /** How well it matched: lower is better. */
  int rank = 0;
};

/**
 * Light up what's round the boxes and edges being looked at: each focused
 * node, the shown edges touching one and the nodes at their other ends, and
 * each focused edge with its two nodes. Nothing is dimmed if nothing's
 * focused.
 *
 * @param graph the graph
 * @param filter what's shown: hidden edges light nothing
 * @param nodes the focused nodes (the hovered box, or the selection)
 * @param edges the focused edges (the hovered arrow, or the arrows bundled
 *        where the mouse is)
 * @param lit set to what's lit, its vectors reused
 */
void light(const Graph& graph,
           const Filter& filter,
           std::span<const uint32_t> nodes,
           std::span<const uint32_t> edges,
           Lit& lit);

/**
 * A node's neighbours by the edges a filter shows.
 *
 * @param graph the graph
 * @param filter what's shown
 * @param centre the node
 * @param out set to its neighbourhood, its vectors reused
 */
void neighbourhood(const Graph& graph,
                   const Filter& filter,
                   uint32_t centre,
                   Neighbourhood& out);

/**
 * The nodes whose names match a search, best first: the whole name, then
 * names starting with it, then names with a word starting with it (`Died`
 * finds `EnemyDiedEvent`), then names holding it anywhere, then names holding
 * its letters in order (`edv` finds `EnemyDiedEvent`). Case is ignored; ties
 * go by name.
 *
 * @param graph the graph
 * @param query what was typed; nothing is found for ""
 * @param limit the most it returns
 * @param hits set to what's found, its vector reused
 */
void search(const Graph& graph,
            std::string_view query,
            std::size_t limit,
            std::vector<SearchHit>& hits);

} // namespace architecture
