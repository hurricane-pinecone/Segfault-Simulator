#include "architecture/layout.h"

#include "architecture/graph.h"
#include "architecture/highlight.h"
#include "architecture/positions.h"
#include "widgets/boardCanvas.h"

#include <imgui.h>
#include <imgui_internal.h> // ImRect

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

using board::operator+;
using board::operator-;

namespace architecture
{

namespace
{

// The columns left to right, one for each kind of node. The data sits
// between the two kinds of code that work on it: game objects to its left,
// systems and services to its right, then the utils and events the systems
// use. Systems beside components, and utils beside systems, keep the most
// arrows shortest (of every order, this one's arrows come within a few
// percent of the shortest, least bent and least crossed). The asset kinds go
// last, as few arrows lead to them.
constexpr std::array<NodeKind, NODE_KINDS> ORDER = {NodeKind::GameObject,
                                                    NodeKind::SceneData,
                                                    NodeKind::Component,
                                                    NodeKind::System,
                                                    NodeKind::Service,
                                                    NodeKind::Utils,
                                                    NodeKind::Event,
                                                    NodeKind::ScriptableObject,
                                                    NodeKind::MeshSource};

// The order the columns are sorted in, and back the other way next round:
// the code first, as it's joined to everything, then what it uses, the
// biggest kinds first.
constexpr std::array<NodeKind, NODE_KINDS> SWEEP = {NodeKind::System,
                                                    NodeKind::Service,
                                                    NodeKind::GameObject,
                                                    NodeKind::Component,
                                                    NodeKind::Utils,
                                                    NodeKind::Event,
                                                    NodeKind::SceneData,
                                                    NodeKind::ScriptableObject,
                                                    NodeKind::MeshSource};

// Space between the whole board's columns, and between rows (canvas units):
// room for the arrows running along them.
constexpr float GUTTER = 360.0f;
constexpr float ROW_GAP = 40.0f;

// Rounds of barycentre sorting: past a dozen or so the order barely changes.
constexpr int SORT_ROUNDS = 16;

// The gap between a neighbourhood's columns, and the most boxes in one.
constexpr float COLUMN_GAP = 200.0f;
constexpr std::size_t PER_COLUMN = 14;

void placeColumns(std::span<const uint32_t> nodes,
                  std::span<const ImVec2> sizes,
                  float start,
                  float direction,
                  float pitch,
                  std::vector<ImVec2>& out);

void placeClear(const Graph& graph,
                std::span<const ImVec2> sizes,
                const Positions& saved,
                std::vector<ImVec2>& out);

} // namespace

void layoutGraph(const Graph& graph,
                 std::span<const ImVec2> sizes,
                 const Positions& saved,
                 std::vector<ImVec2>& out)
{
  const std::size_t count = graph.nodes.size();
  out.assign(count, ImVec2(0.0f, 0.0f));
  std::array<std::vector<uint32_t>, NODE_KINDS> columns;

  // Nodes are sorted by id (kind, then name): each column starts in that
  // order.
  for (uint32_t node = 0; node < count; ++node)
    columns[static_cast<std::size_t>(graph.nodes[node].kind)].push_back(node);

  // Every column's rows on one grid, each column centred on y = 0 to a row:
  // the gaps between rows line up across the board, for the arrows to run
  // along.
  float tallest = 0.0f;

  for (const ImVec2& size : sizes)
    tallest = std::max(tallest, size.y);

  const float pitch = tallest + ROW_GAP;
  std::vector<float> middle(count, 0.0f);
  const auto stack = [&](const std::vector<uint32_t>& column)
  {
    const float top = -static_cast<float>(column.size() / 2) * pitch;

    for (std::size_t i = 0; i < column.size(); ++i)
    {
      const float y = top + static_cast<float>(i) * pitch;
      out[column[i]].y = y;
      middle[column[i]] = y + sizes[column[i]].y * 0.5f;
    }
  };

  for (const std::vector<uint32_t>& column : columns)
    stack(column);

  // Each box goes level with the average of its neighbours in the other
  // columns. The sweep doesn't depend on where the columns go, so neither
  // does the order within them.
  std::vector<float> key(count, 0.0f);

  for (int round = 0; round < SORT_ROUNDS; ++round)
  {
    for (std::size_t step = 0; step < SWEEP.size(); ++step)
    {
      const std::size_t at = round % 2 == 0 ? step : SWEEP.size() - 1 - step;
      const NodeKind kind = SWEEP[at];
      std::vector<uint32_t>& column = columns[static_cast<std::size_t>(kind)];

      for (const uint32_t node : column)
      {
        float sum = 0.0f;
        int neighbours = 0;

        for (const uint32_t touching : graph.edgesOf(node))
        {
          const Edge& edge = graph.edges[touching];
          const uint32_t other = edge.from == node ? edge.to : edge.from;

          if (graph.nodes[other].kind == kind)
            continue;

          sum += middle[other];
          ++neighbours;
        }

        key[node] = neighbours > 0 ? sum / static_cast<float>(neighbours)
                                   : middle[node];
      }

      std::stable_sort(column.begin(),
                       column.end(),
                       [&](uint32_t a, uint32_t b) { return key[a] < key[b]; });
      stack(column);
    }
  }

  float x = 0.0f;

  for (const NodeKind kind : ORDER)
  {
    const std::vector<uint32_t>& column =
        columns[static_cast<std::size_t>(kind)];

    if (column.empty())
      continue;

    float width = 0.0f;

    for (const uint32_t node : column)
    {
      out[node].x = x;
      width = std::max(width, sizes[node].x);
    }

    x += width + GUTTER;
  }

  if (!saved.empty())
    placeClear(graph, sizes, saved, out);
}

void layoutNeighbourhood(const Neighbourhood& around,
                         std::span<const ImVec2> sizes,
                         std::vector<ImVec2>& out)
{
  // Every column's rows on one grid, as the whole board's are, the centre
  // on the middle row.
  float tallest = sizes[around.centre].y;

  for (const auto* side : {&around.incoming, &around.outgoing})
    for (const uint32_t node : *side)
      tallest = std::max(tallest, sizes[node].y);

  const float pitch = tallest + ROW_GAP;
  const ImVec2 size = sizes[around.centre];
  out[around.centre] = ImVec2(-size.x * 0.5f, 0.0f);
  placeColumns(
      around.incoming, sizes, -size.x * 0.5f - COLUMN_GAP, -1.0f, pitch, out);
  placeColumns(
      around.outgoing, sizes, size.x * 0.5f + COLUMN_GAP, 1.0f, pitch, out);
}

namespace
{

// Columns of at most PER_COLUMN boxes, each centred on y = 0 to a row of
// `pitch`, going out from `start` in `direction` (1: rightwards, boxes' left
// sides at the column's start; -1: leftwards, their right sides).
void placeColumns(std::span<const uint32_t> nodes,
                  std::span<const ImVec2> sizes,
                  float start,
                  float direction,
                  float pitch,
                  std::vector<ImVec2>& out)
{
  for (std::size_t first = 0; first < nodes.size(); first += PER_COLUMN)
  {
    const std::span<const uint32_t> column =
        nodes.subspan(first, std::min(PER_COLUMN, nodes.size() - first));
    float width = 0.0f;

    for (const uint32_t node : column)
      width = std::max(width, sizes[node].x);

    float y = -static_cast<float>(column.size() / 2) * pitch;

    for (const uint32_t node : column)
    {
      const float x = direction > 0.0f ? start : start - sizes[node].x;
      out[node] = ImVec2(x, y);
      y += pitch;
    }

    start += direction * (width + COLUMN_GAP);
  }
}

// Saved boxes go where they were; the others keep their place in the layout
// but move down, one at a time, until they're clear of every box placed.
void placeClear(const Graph& graph,
                std::span<const ImVec2> sizes,
                const Positions& saved,
                std::vector<ImVec2>& out)
{
  const std::size_t count = graph.nodes.size();
  std::vector<ImRect> placed;
  std::vector<uint32_t> unsaved;
  placed.reserve(count);

  for (uint32_t node = 0; node < count; ++node)
  {
    const auto it = saved.find(graph.nodes[node].id);

    if (it == saved.end())
    {
      unsaved.push_back(node);
      continue;
    }

    out[node] = it->second;
    placed.emplace_back(out[node], out[node] + sizes[node]);
  }

  // Column by column, top to bottom, so new boxes stack in their layout's
  // order.
  std::sort(unsaved.begin(),
            unsaved.end(),
            [&](uint32_t a, uint32_t b) {
              return std::tie(out[a].x, out[a].y, a) <
                     std::tie(out[b].x, out[b].y, b);
            });

  for (const uint32_t node : unsaved)
  {
    ImRect rect(out[node], out[node] + sizes[node]);

    // Each move goes below a box it overlapped, so it never meets that one
    // again: it's clear after at most one move per box.
    for (bool moved = true; moved;)
    {
      moved = false;

      for (const ImRect& other : placed)
      {
        const ImRect padded(other.Min - ImVec2(0.0f, ROW_GAP),
                            other.Max + ImVec2(0.0f, ROW_GAP));

        if (!padded.Overlaps(rect))
          continue;

        rect.TranslateY(padded.Max.y - rect.Min.y);
        moved = true;
      }
    }

    out[node] = rect.Min;
    placed.push_back(rect);
  }
}

} // namespace

} // namespace architecture
