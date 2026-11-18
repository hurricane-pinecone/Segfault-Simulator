#pragma once

#include "architecture/generator.h"
#include "architecture/graph.h"
#include "architecture/highlight.h"
#include "architecture/layout.h"
#include "architecture/positions.h"
#include "architecture/routing.h"
#include "panels/inspectable.h"
#include "panels/panel.h"
#include "widgets/boardCanvas.h"

#include <imgui.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class BuildEnvironment;

// The Architecture board: the project's systems, services, game objects,
// components, scene data, events and utils as boxes, and what each one's code
// does with the others as arrows, on an endless plane to pan and zoom about.
// It shows the code as it is, tangles and all, so nothing is pruned: instead
// a search finds a box, hovering or selecting one lights up its edges and
// neighbours, a neighbourhood view shows one box's surroundings alone, and
// the kinds of node and edge can be switched off. The selected box's details
// show in the Inspector, and a script opened in the Code panel selects its
// box.
//
// The graph is made on demand by `sfs-lsp --graph` (architecture::Generator):
// when the board opens, when its scripts change while it's open, and on
// Refresh. Nothing runs while it's closed. Box positions are kept per project
// in .sfs/architecture.json.
class ArchitecturePanel : public Panel, public Inspectable
{
public:
  explicit ArchitecturePanel(const BuildEnvironment& environment);

  /** The selected box's details: what its code does, and where. */
  void drawInspector(EditorContext& context) override;

  /** Opens by default only when asked for: it starts a run when it opens. */
  bool openByDefault() const override { return false; }

  void update(EditorContext& context) override;

protected:
  void beforeBegin() override { dockNewBesideViewport(); }
  void drawContents(EditorContext& context) override;
  int windowFlags() const override;

private:
  void applyGenerated(architecture::Generated generated);
  // Show the selected box in the Inspector when the selection changes to
  // one box or a box is clicked; stop when no one box is selected.
  void claimInspector(EditorContext& context);
  void drawBoard();
  void drawBundleTooltip() const;
  void drawEdgeList(EditorContext& context,
                    uint32_t node,
                    bool outgoing,
                    const char* heading);
  void drawEdgeTooltip(uint32_t edge) const;
  // The filters' overlay in the board's top right corner, folded or open.
  void drawFilters(const ImVec2& origin, const ImVec2& size);
  void drawLegend();
  void drawLinks(ImDrawList* draw);
  void drawOverlays(const ImVec2& origin, const ImVec2& size);
  void drawSearchResults(const ImVec2& at, float width);
  void drawToolbar(EditorContext& context);
  // The arrows under the mouse: one, or a trunk's while they're bundled.
  void findHoveredEdges();
  // Select and light the box of a file opened in the Code panel, framed with
  // the boxes it links to (in a neighbourhood: show its neighbourhood).
  void focusFile(const std::filesystem::path& file);
  // Fit a node's box and the shown boxes it links to in view.
  void frameAround(uint32_t node);
  // Fit a node's box in view.
  void frameNode(uint32_t node);
  void handleKeys();
  // Lay every box out afresh (or keep the saved ones), and save.
  void layoutAll(bool keepSaved);
  void leaveNeighbourhood();
  void openSite(EditorContext& context, uint32_t file, int line) const;
  // A search result chosen: select and frame its box, and put its name in
  // the search box, the results closed.
  void pickHit(uint32_t node);
  const std::vector<ImVec2>& positions() const
  {
    return m_neighbourhoodMode ? m_neighbourhoodPositions : m_positions;
  }
  void rebuildView();
  void savePositions();
  // Select one node, showing it if its kind was switched off.
  void selectNode(uint32_t node, bool frame);
  // Show a node's neighbourhood alone, laid out round it; `frame` fits it in
  // view.
  void showNeighbourhood(uint32_t centre, bool frame);
  void startRun(const EditorContext& context);
  void updateLit();

  const BuildEnvironment& m_environment;
  architecture::Generator m_generator;

  std::filesystem::path m_projectRoot; // the project the board is of
  std::filesystem::path m_positionsFile;
  bool m_wasOpen = false;
  uint64_t m_scriptsRevision = 0;
  // Scripts changed: a run starts at this time, unless they change again.
  std::optional<std::chrono::steady_clock::time_point> m_runAt;
  std::optional<architecture::Generated> m_pending; // applied while drawing

  std::optional<architecture::Graph> m_graph;
  std::string m_error; // the last run's, shown over the board

  // By node: its box's size, its kind line, where it is, and whether it
  // shows.
  std::vector<ImVec2> m_sizes;
  std::vector<std::string> m_kindLines;
  std::vector<ImVec2> m_positions;
  std::vector<ImVec2> m_neighbourhoodPositions;
  std::vector<uint8_t> m_shown;
  architecture::Positions m_saved;
  bool m_dragged = false; // boxes moved since the drag began
  std::array<std::size_t, architecture::NODE_KINDS> m_nodeCounts{};
  std::array<std::size_t, architecture::EDGE_KINDS> m_edgeCounts{};

  // What's drawn: the shown boxes (box id = node index + 1) and their
  // rectangles, and each shown edge as a link routed between them.
  std::vector<board::Box> m_boxes;
  std::vector<ImRect> m_rects;
  std::vector<uint32_t> m_boxOf; // by node: its box, or UINT32_MAX if hidden
  std::vector<architecture::RouteLink> m_routeLinks;
  std::vector<uint32_t> m_linkEdges; // by link: its edge
  architecture::Router m_router;
  architecture::Routes m_routes;
  // By route piece: its colour and width while the arrows are bundled.
  std::vector<ImU32> m_pieceColors;
  std::vector<float> m_pieceWidths;
  std::vector<ImVec2> m_path; // a link's path as it's drawn, reused
  board::Canvas m_canvas;

  architecture::Filter m_filter;
  architecture::Lit m_lit;
  std::vector<uint32_t> m_focus;    // the nodes lit up round
  std::vector<uint32_t> m_litFocus; // what m_lit was worked out for
  std::vector<uint32_t> m_litEdges;
  bool m_litStale = true;
  std::vector<uint32_t> m_hoveredEdges;

  bool m_neighbourhoodMode = false;
  architecture::Neighbourhood m_neighbourhood;
  std::vector<uint8_t> m_inNeighbourhood;

  std::string m_query;
  std::vector<architecture::SearchHit> m_hits;
  int m_hitChoice = 0;
  bool m_searchActive = false;
  bool m_searchFocus = false; // focus the search box next frame
  bool m_resultsHovered = false;
  float m_searchLeft = 0.0f; // the search box's left side, on screen
  // The box last chosen from the search, its name in the search box while
  // it's the one selected.
  std::optional<uint32_t> m_picked;

  bool m_filtersOpen = false;

  // The selection the Inspector was last given or not, and whether a box
  // was clicked since (which gives it the Inspector again).
  std::vector<board::BoxId> m_inspectedSelection;
  bool m_boxClicked = false;

  // The Code panel's script, as last seen, and one opened since to show.
  uint64_t m_codeRevision = 0;
  std::optional<std::filesystem::path> m_focusFile;
};
