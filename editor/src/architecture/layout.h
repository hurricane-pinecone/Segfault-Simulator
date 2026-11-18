#pragma once

#include "architecture/graph.h"
#include "architecture/highlight.h"
#include "architecture/positions.h"

#include <imgui.h>

#include <span>
#include <vector>

// Where the Architecture board puts its boxes: the whole graph in one column
// per kind of node, or one node's neighbourhood round it.
//
// The whole graph reads left to right, each kind in a single column as tall
// as it needs: game objects, scene data, components, systems, services, utils,
// events, then scriptable objects and mesh sources. The data sits between the
// two kinds of code that work on it, and the systems between the data and the
// utils and events they use, so the most arrows are short (see layout.cpp for
// how the order was picked). Every column's rows are on one grid, with gutters
// between the columns and gaps between the rows for the arrows to run along
// (see routing.h). Each column's order comes from a few rounds of barycentre
// sorting (each box goes level with the average of its neighbours in the other
// columns), so edges cross less. It's deterministic: the same graph and box
// sizes always give the same layout.
namespace architecture
{

/**
 * Lay out every node: one with a saved position keeps it; the rest go where
 * the column layout puts them, moved down where they'd overlap a box
 * already placed. With nothing saved, it's the column layout alone.
 *
 * @param graph the graph
 * @param sizes each node's box size, by index into Graph::nodes
 * @param saved positions kept from before, by node id
 * @param out set to each node's box's top left corner, by index
 */
void layoutGraph(const Graph& graph,
                 std::span<const ImVec2> sizes,
                 const Positions& saved,
                 std::vector<ImVec2>& out);

/**
 * Lay out a neighbourhood round its centre: the centre's top middle at the
 * canvas origin, what points at it in columns to its left and what it points
 * at to its right, each column centred on it, all on one grid of rows. Only the
 * neighbourhood's entries in `out` are written.
 *
 * @param around the neighbourhood
 * @param sizes each node's box size, by index into Graph::nodes
 * @param out each node's box's top left corner, by index; sized to the
 *        graph's nodes
 */
void layoutNeighbourhood(const Neighbourhood& around,
                         std::span<const ImVec2> sizes,
                         std::vector<ImVec2>& out);

} // namespace architecture
