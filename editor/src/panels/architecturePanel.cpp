#include "panels/architecturePanel.h"

#include "architecture/generator.h"
#include "architecture/graph.h"
#include "architecture/highlight.h"
#include "architecture/layout.h"
#include "architecture/positions.h"
#include "architecture/routing.h"
#include "build/buildEnvironment.h"
#include "editorContext.h"
#include "project/project.h"
#include "widgets/boardCanvas.h"
#include "widgets/widgets.h"

#include <imgui.h>
#include <imgui_internal.h> // ImRect, GetActiveID

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace arch = architecture;

using board::operator+;
using board::operator-;
using board::operator*;

namespace
{

// Scripts saved in quick succession make one run, this long after the last.
constexpr std::chrono::milliseconds RERUN_DELAY{1000};

// The most a search lists, and its box's width.
constexpr std::size_t SEARCH_LIMIT = 50;
constexpr float SEARCH_WIDTH = 280.0f;

// The filters' overlay's width, open.
constexpr float FILTERS_WIDTH = 380.0f;

// How close the mouse must be to a link to hover it (pixels).
constexpr float LINK_HIT = 5.0f;

// A link's alpha while nothing is looked at: the tangle shows, the boxes read.
constexpr int RESTING_ALPHA = 150;

// Zoomed out past the first, the arrows are bundled into trunks; zoomed in
// past the second, each has a lane of its own; between, they spread apart.
constexpr float BUNDLED_ZOOM = 0.3f;
constexpr float SPREAD_ZOOM = 0.7f;

// A trunk carrying arrows of mixed kinds, none the most of them.
constexpr ImU32 TRUNK_COLOR = IM_COL32(170, 172, 166, 255);

// The most arrows a trunk's tooltip lists.
constexpr std::size_t TOOLTIP_EDGES = 16;

// The most sites a hovered link's tooltip lists.
constexpr std::size_t TOOLTIP_SITES = 12;

// Room round a box framed on its own, so its neighbours show (canvas units).
constexpr ImVec2 FRAME_ROOM{320.0f, 200.0f};

// Each kind's colour: a box's header, an arrow's line.
constexpr std::array<ImU32, arch::NODE_KINDS> NODE_COLORS = {
    IM_COL32(70, 98, 136, 255),  // system
    IM_COL32(48, 112, 120, 255), // service
    IM_COL32(86, 122, 78, 255),  // gameObject
    IM_COL32(118, 92, 64, 255),  // component
    IM_COL32(122, 74, 98, 255),  // sceneData
    IM_COL32(138, 106, 40, 255), // event
    IM_COL32(98, 84, 130, 255),  // scriptableObject
    IM_COL32(76, 100, 100, 255), // meshSource
    IM_COL32(88, 88, 88, 255),   // utils
};

constexpr std::array<ImU32, arch::EDGE_KINDS> EDGE_COLORS = {
    IM_COL32(132, 152, 184, 255), // registers
    IM_COL32(96, 182, 182, 255),  // observes
    IM_COL32(112, 198, 120, 255), // reads
    IM_COL32(242, 128, 72, 255),  // writes
    IM_COL32(234, 206, 84, 255),  // adds
    IM_COL32(216, 96, 172, 255),  // removes
    IM_COL32(150, 150, 150, 255), // filters
    IM_COL32(92, 152, 246, 255),  // sends
    IM_COL32(84, 208, 234, 255),  // consumes
    IM_COL32(170, 130, 246, 255), // requests
    IM_COL32(246, 152, 208, 255), // responds
    IM_COL32(246, 64, 64, 255),   // reads-system
    IM_COL32(178, 148, 106, 255), // uses-util
};

constexpr std::array<const char*, arch::NODE_KINDS> NODE_LABELS = {
    "Systems",
    "Services",
    "Game objects",
    "Components",
    "Scene data",
    "Events",
    "Scriptable objects",
    "Mesh sources",
    "Utils",
};

// An engine node's header: its kind's colour, greyed towards the board.
constexpr ImU32 ENGINE_GREY = IM_COL32(60, 62, 61, 255);

// The background behind an overlay on the board.
constexpr ImVec4 OVERLAY_BACKGROUND{0.12f, 0.13f, 0.125f, 0.92f};

constexpr ImVec4 ERROR_TEXT{0.95f, 0.5f, 0.45f, 1.0f};

ImRect boundsOf(std::span<const board::Box> boxes);

bool drawKindToggle(const char* label,
                    ImU32 color,
                    bool line,
                    std::size_t count,
                    bool& on);

ImU32 headerColor(const arch::Node& node);

float spreadAt(float zoom);

std::string kindLine(const arch::Node& node);

const char* phaseLabel(std::string_view phase);

ImU32 withAlpha(ImU32 color, int alpha);

} // namespace

ArchitecturePanel::ArchitecturePanel(const BuildEnvironment& environment)
    : Panel("Architecture"), m_environment(environment)
{
  open() = false;
  board::Settings& settings = m_canvas.settings();
  settings.minZoom = 0.03f;
  settings.fitMaxZoom = 1.0f;
}

void ArchitecturePanel::update(EditorContext& context)
{
  const bool opened = open() && !m_wasOpen;
  m_wasOpen = open();

  // Scripts opened in the Code panel while it's closed aren't shown.
  const bool codeOpened = context.codeScriptRevision != m_codeRevision;
  m_codeRevision = context.codeScriptRevision;

  if (!open())
  {
    m_generator.stop();
    m_runAt.reset();

    if (context.inspected == this)
      context.inspected = nullptr;

    return;
  }

  if (codeOpened && !context.codeScript.empty())
    m_focusFile = context.codeScript;

  const std::filesystem::path root =
      context.project ? context.project->root() : std::filesystem::path();

  if (root != m_projectRoot)
  {
    m_generator.stop();
    m_projectRoot = root;
    m_positionsFile = root.empty() ? root : root / ".sfs" / "architecture.json";
    m_saved =
        root.empty() ? arch::Positions() : arch::readPositions(m_positionsFile);
    m_graph.reset();
    m_pending.reset();
    m_error.clear();
    m_boxes.clear();
    m_rects.clear();
    m_routeLinks.clear();
    m_linkEdges.clear();
    m_neighbourhoodMode = false;
    m_canvas.clearSelection();
    m_hits.clear();
    m_scriptsRevision = context.scriptsRevision;
    m_runAt.reset();

    if (!root.empty())
      startRun(context);
  }
  else if (opened && !root.empty())
    startRun(context);

  const auto now = std::chrono::steady_clock::now();

  if (context.scriptsRevision != m_scriptsRevision)
  {
    m_scriptsRevision = context.scriptsRevision;
    m_runAt = now + RERUN_DELAY;
  }

  if (m_runAt && now >= *m_runAt && !root.empty())
    startRun(context);

  if (std::optional<arch::Generated> done = m_generator.poll())
    m_pending = std::move(*done);
}

void ArchitecturePanel::drawContents(EditorContext& context)
{
  // Not mid-drag: the boxes being moved are named by the graph's indices.
  if (m_pending && !m_canvas.busy())
    applyGenerated(*std::exchange(m_pending, std::nullopt));

  handleKeys();
  drawToolbar(context);
  ImGui::Separator();

  if (!context.project)
  {
    ImGui::TextDisabled("Open a project to see its architecture.");
    return;
  }

  // A script opened in the Code panel shows its box, unless the board is
  // being searched (it waits while boxes are dragged).
  if (m_focusFile && m_graph && !m_canvas.busy())
  {
    if (!m_searchActive)
      focusFile(*m_focusFile);

    m_focusFile.reset();
  }

  // The name chosen from the search stays while its box is the one selected.
  const std::vector<board::BoxId>& selection = m_canvas.selection();

  if (m_picked && (selection.size() != 1 || selection.front() != *m_picked + 1))
  {
    if (m_graph && m_query == m_graph->nodes[*m_picked].name)
    {
      m_query.clear();
      m_hits.clear();
    }

    m_picked.reset();
  }

  ImGui::BeginChild("Board",
                    ImVec2(0.0f, 0.0f),
                    ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse |
                        ImGuiWindowFlags_NoMove);
  drawBoard();
  ImGui::EndChild();
  claimInspector(context);
}

void ArchitecturePanel::drawInspector(EditorContext& context)
{
  const std::vector<board::BoxId>& selection = m_canvas.selection();

  if (!m_graph || selection.size() != 1)
  {
    ImGui::TextDisabled("Nothing selected");
    return;
  }

  const uint32_t node = static_cast<uint32_t>(selection.front() - 1);
  const arch::Node& shown = m_graph->nodes[node];
  ImGui::SeparatorText(shown.name.c_str());
  ImGui::TextDisabled("%s", m_kindLines[node].c_str());

  if (shown.qualifiedName != shown.name)
    ImGui::TextDisabled("%s", shown.qualifiedName.c_str());

  if (shown.file != arch::NO_FILE)
  {
    char label[512];
    std::snprintf(label,
                  sizeof(label),
                  "%s:%d##declared",
                  m_graph->files[shown.file].c_str(),
                  shown.line);

    if (ImGui::Selectable(label))
      openSite(context, shown.file, shown.line);

    ImGui::SetItemTooltip("Open where it's declared");
  }

  drawEdgeList(context, node, true, "What its code does");
  drawEdgeList(context, node, false, "What's done to it");
}

int ArchitecturePanel::windowFlags() const
{
  return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
}

void ArchitecturePanel::applyGenerated(arch::Generated generated)
{
  if (!generated.graph)
  {
    m_error = std::move(generated.error);
    return;
  }

  m_error.clear();
  const bool first = !m_graph;

  // The selection and neighbourhood, by id: the indices change.
  std::vector<std::string> selected;
  std::optional<std::string> centre;

  if (m_graph)
  {
    for (const board::BoxId id : m_canvas.selection())
      selected.push_back(m_graph->nodes[id - 1].id);

    if (m_neighbourhoodMode)
      centre = m_graph->nodes[m_neighbourhood.centre].id;
  }

  m_graph = std::move(*generated.graph);
  const arch::Graph& graph = *m_graph;
  const std::size_t count = graph.nodes.size();

  m_kindLines.resize(count);
  m_sizes.resize(count);
  m_nodeCounts.fill(0);
  m_edgeCounts.fill(0);

  for (std::size_t i = 0; i < count; ++i)
  {
    const arch::Node& node = graph.nodes[i];
    m_kindLines[i] = kindLine(node);
    m_sizes[i] = board::boxSize(
        m_canvas.style(), node.name, std::span(&m_kindLines[i], 1));
    ++m_nodeCounts[static_cast<std::size_t>(node.kind)];
  }

  for (const arch::Edge& edge : graph.edges)
    ++m_edgeCounts[static_cast<std::size_t>(edge.kind)];

  m_canvas.clearSelection();

  for (const std::string& id : selected)
    if (const std::optional<uint32_t> node = graph.find(id))
      m_canvas.selectAlso(*node + 1);

  m_inNeighbourhood.assign(count, 0);
  m_neighbourhoodPositions.assign(count, ImVec2(0.0f, 0.0f));
  m_neighbourhoodMode = false;
  layoutAll(true);

  if (centre)
    if (const std::optional<uint32_t> node = graph.find(*centre))
      showNeighbourhood(*node, false);

  arch::search(graph, m_query, SEARCH_LIMIT, m_hits);
  m_hitChoice = 0;

  if (first)
    m_canvas.frame();
}

void ArchitecturePanel::claimInspector(EditorContext& context)
{
  const std::vector<board::BoxId>& selection = m_canvas.selection();
  const bool changed = selection != m_inspectedSelection;
  m_inspectedSelection = selection;
  const bool clicked = std::exchange(m_boxClicked, false);

  if (!m_graph || selection.size() != 1)
  {
    if (context.inspected == this)
      context.inspected = nullptr;

    return;
  }

  if (changed || clicked)
    context.inspected = this;
}

void ArchitecturePanel::drawBoard()
{
  if (!m_canvas.begin("board"))
    return;

  const ImVec2 origin = m_canvas.view().origin;
  const ImVec2 size = m_canvas.view().size;
  m_canvas.applyFrame(m_boxes);
  const board::Input input = m_canvas.handleMouse(m_boxes);
  m_boxClicked |= m_canvas.hoveredBox() != 0 && ImGui::IsItemHovered() &&
                  ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  findHoveredEdges();

  if (!input.moved.empty())
  {
    std::vector<ImVec2>& at =
        m_neighbourhoodMode ? m_neighbourhoodPositions : m_positions;

    for (const board::BoxMove& move : input.moved)
    {
      ImVec2& position = at[move.id - 1];
      m_dragged |= position.x != move.min.x || position.y != move.min.y;
      position = move.min;
    }

    rebuildView();
  }

  // Where the boxes were let go is kept (a neighbourhood's layout isn't).
  if (input.moveEnded && std::exchange(m_dragged, false) &&
      !m_neighbourhoodMode)
  {
    for (const board::BoxId id : m_canvas.selection())
      m_saved[m_graph->nodes[id - 1].id] = m_positions[id - 1];

    savePositions();
  }

  if (input.doubleClickedBox)
    showNeighbourhood(static_cast<uint32_t>(input.doubleClickedBox - 1), true);

  ImDrawList* draw = m_canvas.beginDraw();

  if (m_graph)
  {
    updateLit();
    const arch::Graph& graph = *m_graph;

    // The links under the boxes.
    drawLinks(draw);

    for (const board::Box& box : m_boxes)
    {
      const std::size_t node = box.id - 1;
      m_canvas.drawBox(draw,
                       box,
                       {.title = graph.nodes[node].name,
                        .lines = std::span(&m_kindLines[node], 1),
                        .header = headerColor(graph.nodes[node]),
                        .dimmed = m_lit.active && !m_lit.nodes[node],
                        .hovered = m_canvas.hoveredBox() == box.id,
                        .selected = m_canvas.isSelected(box.id)});
    }
  }

  m_canvas.endDraw(draw);

  if (m_hoveredEdges.size() == 1)
    drawEdgeTooltip(m_hoveredEdges.front());
  else if (!m_hoveredEdges.empty())
    drawBundleTooltip();

  drawOverlays(origin, size);
}

void ArchitecturePanel::drawBundleTooltip() const
{
  const arch::Graph& graph = *m_graph;
  const std::vector<uint32_t>& edges = m_hoveredEdges;
  const uint32_t from = graph.edges[edges.front()].from;
  const uint32_t to = graph.edges[edges.front()].to;
  bool oneFrom = true;
  bool oneTo = true;
  std::array<bool, arch::EDGE_KINDS> kinds{};

  for (const uint32_t edge : edges)
  {
    oneFrom &= graph.edges[edge].from == from;
    oneTo &= graph.edges[edge].to == to;
    kinds[static_cast<std::size_t>(graph.edges[edge].kind)] = true;
  }

  if (!ImGui::BeginTooltip())
    return;

  // Whose trunk it is: the box all its arrows leave, or all enter.
  const auto kindCount =
      static_cast<std::size_t>(std::ranges::count(kinds, true));
  const char* plural = kindCount == 1 ? "" : "s";

  if (oneFrom)
    ImGui::Text("%s: %zu arrows, %zu kind%s",
                graph.nodes[from].name.c_str(),
                edges.size(),
                kindCount,
                plural);
  else if (oneTo)
    ImGui::Text("%zu arrows, %zu kind%s, into %s",
                edges.size(),
                kindCount,
                plural,
                graph.nodes[to].name.c_str());
  else
    ImGui::Text("%zu arrows, %zu kind%s", edges.size(), kindCount, plural);

  ImGui::Separator();

  for (std::size_t i = 0; i < edges.size() && i < TOOLTIP_EDGES; ++i)
  {
    const arch::Edge& edge = graph.edges[edges[i]];
    const std::string_view kind = arch::edgeKindName(edge.kind);
    ImGui::TextUnformatted(graph.nodes[edge.from].name.c_str());
    ImGui::SameLine();
    ImGui::PushStyleColor(
        ImGuiCol_Text, EDGE_COLORS[static_cast<std::size_t>(edge.kind)]);
    ImGui::Text("%.*s", static_cast<int>(kind.size()), kind.data());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(graph.nodes[edge.to].name.c_str());
  }

  if (edges.size() > TOOLTIP_EDGES)
    ImGui::TextDisabled("and %zu more (zoom in to tell them apart)",
                        edges.size() - TOOLTIP_EDGES);

  ImGui::EndTooltip();
}

void ArchitecturePanel::drawEdgeList(EditorContext& context,
                                     uint32_t node,
                                     bool outgoing,
                                     const char* heading)
{
  const arch::Graph& graph = *m_graph;
  ImGui::SeparatorText(heading);
  bool any = false;

  for (std::size_t kind = 0; kind < arch::EDGE_KINDS; ++kind)
  {
    int count = 0;

    for (const uint32_t touching : graph.edgesOf(node))
    {
      const arch::Edge& edge = graph.edges[touching];

      if (static_cast<std::size_t>(edge.kind) == kind &&
          (outgoing ? edge.from : edge.to) == node &&
          m_filter.shows(graph, edge))
        ++count;
    }

    if (count == 0)
      continue;

    any = true;
    const std::string_view name =
        arch::edgeKindName(static_cast<arch::EdgeKind>(kind));
    ImGui::PushID(static_cast<int>(kind) + (outgoing ? 0 : 100));
    ImGui::PushStyleColor(ImGuiCol_Text, EDGE_COLORS[kind]);
    const bool open = ImGui::TreeNodeEx("kind",
                                        ImGuiTreeNodeFlags_DefaultOpen,
                                        "%s%.*s (%d)",
                                        outgoing ? "" : "by: ",
                                        static_cast<int>(name.size()),
                                        name.data(),
                                        count);
    ImGui::PopStyleColor();

    if (!open)
    {
      ImGui::PopID();
      continue;
    }

    for (const uint32_t touching : graph.edgesOf(node))
    {
      const arch::Edge& edge = graph.edges[touching];

      if (static_cast<std::size_t>(edge.kind) != kind ||
          (outgoing ? edge.from : edge.to) != node ||
          !m_filter.shows(graph, edge))
        continue;

      const uint32_t other = outgoing ? edge.to : edge.from;
      ImGui::PushID(static_cast<int>(touching));

      if (ImGui::Selectable(graph.nodes[other].name.c_str()))
        selectNode(other, true);

      ImGui::SetItemTooltip(
          "Select and show %s", graph.nodes[other].name.c_str());
      ImGui::Indent();

      for (std::size_t i = 0; i < edge.sites.size(); ++i)
      {
        const arch::Site& site = edge.sites[i];
        char label[512];
        std::snprintf(label,
                      sizeof(label),
                      "%s:%d  %s%s%s##%zu",
                      graph.files[site.file].c_str(),
                      site.line,
                      site.via.c_str(),
                      site.term.empty() ? "" : " ",
                      site.term.c_str(),
                      i);
        ImGui::PushStyleColor(
            ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

        if (ImGui::Selectable(label))
          openSite(context, site.file, site.line);

        ImGui::PopStyleColor();
      }

      ImGui::Unindent();
      ImGui::PopID();
    }

    ImGui::TreePop();
    ImGui::PopID();
  }

  if (!any)
    ImGui::TextDisabled("Nothing shown.");
}

void ArchitecturePanel::drawEdgeTooltip(uint32_t edge) const
{
  const arch::Graph& graph = *m_graph;
  const arch::Edge& link = graph.edges[edge];
  const std::string_view kind = arch::edgeKindName(link.kind);

  if (!ImGui::BeginTooltip())
    return;

  ImGui::TextUnformatted(graph.nodes[link.from].name.c_str());
  ImGui::SameLine();
  ImGui::PushStyleColor(
      ImGuiCol_Text, EDGE_COLORS[static_cast<std::size_t>(link.kind)]);
  ImGui::Text("%.*s", static_cast<int>(kind.size()), kind.data());
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextUnformatted(graph.nodes[link.to].name.c_str());
  ImGui::Separator();

  for (std::size_t i = 0; i < link.sites.size() && i < TOOLTIP_SITES; ++i)
  {
    const arch::Site& site = link.sites[i];
    ImGui::TextDisabled("%s:%d  %s%s%s",
                        graph.files[site.file].c_str(),
                        site.line,
                        site.via.c_str(),
                        site.term.empty() ? "" : " ",
                        site.term.c_str());
  }

  if (link.sites.size() > TOOLTIP_SITES)
    ImGui::TextDisabled("and %zu more (select a box to list them all)",
                        link.sites.size() - TOOLTIP_SITES);

  ImGui::EndTooltip();
}

void ArchitecturePanel::drawFilters(const ImVec2& origin, const ImVec2& size)
{
  constexpr float MARGIN = 8.0f;

  // Folded, it says how many kinds are switched off.
  const auto off = std::ranges::count(m_filter.nodeKinds, false) +
                   std::ranges::count(m_filter.edgeKinds, false);
  char label[64];

  if (off > 0)
    std::snprintf(
        label, sizeof(label), "Filters (%d off)", static_cast<int>(off));
  else
    std::snprintf(label, sizeof(label), "Filters");

  const ImGuiStyle& style = ImGui::GetStyle();
  const float frame = ImGui::GetFrameHeight();
  const float folded = ImGui::CalcTextSize(label).x + frame +
                       style.FramePadding.x * 2.0f +
                       style.WindowPadding.x * 2.0f;
  const float width =
      std::min(m_filtersOpen ? FILTERS_WIDTH : folded, size.x - 2.0f * MARGIN);
  ImGui::SetCursorScreenPos(
      ImVec2(origin.x + size.x - width - MARGIN, origin.y + MARGIN));

  if (ImGui::BeginChild("Filters",
                        ImVec2(width, 0.0f),
                        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                            ImGuiChildFlags_AlwaysUseWindowPadding))
  {
    // A header that toggles on release only. A CollapsingHeader's arrow
    // toggles on press, and the overlay widening leftwards put the label
    // under the held mouse, whose release toggled it shut again.
    const ImVec2 at = ImGui::GetCursorScreenPos();

    if (ImGui::Selectable("##filters", true, 0, ImVec2(0.0f, frame)))
      m_filtersOpen = !m_filtersOpen;

    ImGui::SetItemTooltip("Which kinds of box and arrow show");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float textY = at.y + (frame - ImGui::GetFontSize()) * 0.5f;
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::RenderArrow(draw,
                       ImVec2(at.x + style.FramePadding.x, textY),
                       text,
                       m_filtersOpen ? ImGuiDir_Down : ImGuiDir_Right);
    draw->AddText(ImVec2(at.x + style.FramePadding.x + ImGui::GetFontSize() +
                             style.ItemInnerSpacing.x,
                         textY),
                  text,
                  label);

    if (m_filtersOpen)
      drawLegend();
  }

  ImGui::EndChild();
}

void ArchitecturePanel::drawLegend()
{
  bool changed = false;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 2.0f));

  // Boxes, then arrows: each kind's switch and colour; a right click shows
  // that kind alone, or every kind again.
  const auto table = [&](const char* id,
                         std::span<bool> on,
                         std::span<const ImU32> colors,
                         std::span<const std::size_t> counts,
                         bool lines,
                         const auto& labelOf)
  {
    if (!ImGui::BeginTable(id, 2))
      return;

    for (std::size_t kind = 0; kind < on.size(); ++kind)
    {
      ImGui::TableNextColumn();
      ImGui::PushID(static_cast<int>(kind));
      changed |= drawKindToggle(
          labelOf(kind), colors[kind], lines, counts[kind], on[kind]);

      if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
      {
        const bool alone = std::ranges::count(on, true) == 1 && on[kind];
        std::ranges::fill(on, alone);
        on[kind] = true;
        changed = true;
      }

      ImGui::SetItemTooltip("Right-click to show this kind alone");
      ImGui::PopID();
    }

    ImGui::EndTable();
  };

  table("Boxes",
        m_filter.nodeKinds,
        NODE_COLORS,
        m_nodeCounts,
        false,
        [](std::size_t kind) { return NODE_LABELS[kind]; });
  ImGui::Spacing();
  table("Arrows",
        m_filter.edgeKinds,
        EDGE_COLORS,
        m_edgeCounts,
        true,
        [](std::size_t kind) {
          return arch::edgeKindName(static_cast<arch::EdgeKind>(kind)).data();
        });
  ImGui::PopStyleVar(2);
  ImGui::Spacing();

  if (!changed || !m_graph)
    return;

  if (m_neighbourhoodMode)
  {
    showNeighbourhood(m_neighbourhood.centre, false);
    return;
  }

  rebuildView();
}

void ArchitecturePanel::drawLinks(ImDrawList* draw)
{
  const arch::Graph& graph = *m_graph;
  const board::View& view = m_canvas.view();
  const float spread = spreadAt(view.zoom);

  // Zoomed out, the trunks: each stretch of channel once, thicker the more
  // arrows it carries, fading out as they spread into lanes.
  if (spread < 1.0f)
  {
    const int alpha = static_cast<int>(RESTING_ALPHA * (1.0f - spread));

    for (std::size_t p = 0; p < m_routes.pieces.size(); ++p)
    {
      const arch::RoutePiece& piece = m_routes.pieces[p];
      const std::array<ImVec2, 2> ends = {piece.a, piece.b};
      m_canvas.drawPath(draw,
                        ends,
                        {.arrow = piece.entersBox,
                         .color = withAlpha(m_pieceColors[p], alpha),
                         .cornerRadius = 0.0f,
                         .dimmed = m_lit.active,
                         .width = m_pieceWidths[p]});
    }
  }

  // Each arrow in its own lane and its kind's colour as they spread, and the
  // lit ones whole (bundled or not) over the rest.
  for (const bool litPass : {false, true})
    for (uint32_t link = 0; link < m_routeLinks.size(); ++link)
    {
      const uint32_t edge = m_linkEdges[link];
      const bool lit = m_lit.active && m_lit.edges[edge];

      if (lit != litPass || (!lit && spread <= 0.0f))
        continue;

      const ImRect& bounds = m_routes.bounds[link];

      if (!view.shows(bounds.Min, bounds.Max, 12.0f))
        continue;

      arch::routePath(m_routes, link, spread, m_path);
      const ImU32 color =
          EDGE_COLORS[static_cast<std::size_t>(graph.edges[edge].kind)];
      const bool hovered =
          std::ranges::find(m_hoveredEdges, edge) != m_hoveredEdges.end();
      const float litWidth = hovered ? 3.5f : 2.5f;
      const int restingAlpha = static_cast<int>(RESTING_ALPHA * spread);
      m_canvas.drawPath(draw,
                        m_path,
                        {.arrow = true,
                         .color = lit ? color : withAlpha(color, restingAlpha),
                         .dimmed = m_lit.active && !lit,
                         .width = lit ? litWidth : 1.5f});
    }
}

void ArchitecturePanel::drawOverlays(const ImVec2& origin, const ImVec2& size)
{
  constexpr float MARGIN = 8.0f;
  const ImGuiChildFlags overlay = ImGuiChildFlags_Borders |
                                  ImGuiChildFlags_AutoResizeY |
                                  ImGuiChildFlags_AlwaysUseWindowPadding;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, OVERLAY_BACKGROUND);

  if (!m_error.empty())
  {
    const float width = std::min(size.x - 2.0f * MARGIN, 640.0f);
    ImGui::SetCursorScreenPos(origin + ImVec2(MARGIN, MARGIN));

    if (ImGui::BeginChild("Error", ImVec2(width, 0.0f), overlay))
    {
      ImGui::PushStyleColor(ImGuiCol_Text, ERROR_TEXT);
      ImGui::TextWrapped("%s", m_error.c_str());
      ImGui::PopStyleColor();

      if (m_graph)
        ImGui::TextDisabled("The board shows the last graph made.");
    }

    ImGui::EndChild();
  }

  if (m_generator.running())
  {
    const arch::Progress progress = m_generator.progress();
    const float width = 300.0f;
    const float height = ImGui::GetFrameHeightWithSpacing() * 2.0f +
                         ImGui::GetStyle().WindowPadding.y * 2.0f;
    ImGui::SetCursorScreenPos(
        ImVec2(origin.x + MARGIN, origin.y + size.y - height - MARGIN));

    if (ImGui::BeginChild("Progress", ImVec2(width, 0.0f), overlay))
    {
      ImGui::Text("%s the scripts: %d / %d",
                  phaseLabel(progress.phase),
                  progress.done,
                  progress.total);
      const float fraction = progress.total > 0
                                 ? static_cast<float>(progress.done) /
                                       static_cast<float>(progress.total)
                                 : 0.0f;
      ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), "");
    }

    ImGui::EndChild();
  }

  if (m_graph)
    drawFilters(origin, size);

  if ((m_searchActive || m_resultsHovered) && !m_query.empty())
    drawSearchResults(ImVec2(m_searchLeft, origin.y + 2.0f), SEARCH_WIDTH);
  else
    m_resultsHovered = false;

  ImGui::PopStyleColor();

  if (!m_graph && m_generator.running())
  {
    const char* text = "Reading the project's scripts...";
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    ImGui::SetCursorScreenPos(origin + (size - textSize) * 0.5f);
    ImGui::TextDisabled("%s", text);
  }
}

void ArchitecturePanel::drawSearchResults(const ImVec2& at, float width)
{
  ImGui::SetCursorScreenPos(at);
  const ImGuiChildFlags flags = ImGuiChildFlags_Borders |
                                ImGuiChildFlags_AutoResizeY |
                                ImGuiChildFlags_AlwaysUseWindowPadding;

  if (!ImGui::BeginChild("Results", ImVec2(width, 0.0f), flags))
  {
    ImGui::EndChild();
    return;
  }

  m_resultsHovered =
      ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                             ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

  if (m_hits.empty())
    ImGui::TextDisabled("No box is named like that.");

  for (std::size_t i = 0; i < m_hits.size(); ++i)
  {
    const arch::Node& node = m_graph->nodes[m_hits[i].node];
    ImGui::PushID(static_cast<int>(i));

    if (ImGui::Selectable(
            node.name.c_str(), static_cast<int>(i) == m_hitChoice))
      pickHit(m_hits[i].node);

    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_kindLines[m_hits[i].node].c_str());
    ImGui::PopID();
  }

  ImGui::EndChild();
}

void ArchitecturePanel::drawToolbar(EditorContext& context)
{
  // The search box's id, as widgets::searchInput() makes it, to tell
  // whether it's being typed in.
  ImGui::PushID("search");
  const ImGuiID searchId = ImGui::GetID("##search");
  ImGui::PopID();
  const bool wasActive = m_searchActive;

  if (std::exchange(m_searchFocus, false))
    ImGui::SetKeyboardFocusHere();

  // The box's place, for the results under it (its last item is its clear
  // button).
  m_searchLeft = ImGui::GetCursorScreenPos().x;

  if (widgets::searchInput(
          "search", m_query, "Search boxes (Ctrl+F)", SEARCH_WIDTH) &&
      m_graph)
  {
    arch::search(*m_graph, m_query, SEARCH_LIMIT, m_hits);
    m_hitChoice = 0;
    m_picked.reset();
  }

  m_searchActive = ImGui::GetActiveID() == searchId;
  const int hits = static_cast<int>(m_hits.size());

  if (m_searchActive && hits > 0)
  {
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
      m_hitChoice = std::min(m_hitChoice + 1, hits - 1);

    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
      m_hitChoice = std::max(m_hitChoice - 1, 0);
  }

  if (wasActive && hits > 0 &&
      (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
    pickHit(m_hits[std::clamp(m_hitChoice, 0, hits - 1)].node);

  ImGui::SameLine();
  ImGui::BeginDisabled(!context.project);

  if (ImGui::Button("Refresh"))
    startRun(context);

  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Read the scripts again (it's done by itself as "
                        "they're saved)");
  ImGui::SameLine();
  ImGui::BeginDisabled(!m_graph);

  if (ImGui::Button("Re-layout"))
  {
    if (m_neighbourhoodMode)
      showNeighbourhood(m_neighbourhood.centre, true);
    else
    {
      layoutAll(false);
      m_canvas.frame();
    }
  }

  ImGui::SetItemTooltip("Lay every box out afresh, forgetting where they "
                        "were moved");
  ImGui::SameLine();

  if (ImGui::Button("Frame"))
    m_canvas.frame();

  ImGui::SetItemTooltip("Fit the selected boxes, or every box, in view (F)");
  ImGui::EndDisabled();
  ImGui::SameLine();

  bool neighbourhood = m_neighbourhoodMode;
  ImGui::BeginDisabled(!m_neighbourhoodMode &&
                       m_canvas.selection().size() != 1);

  if (ImGui::Checkbox("Neighbourhood", &neighbourhood))
  {
    if (neighbourhood)
      showNeighbourhood(
          static_cast<uint32_t>(m_canvas.selection().front() - 1), true);
    else
      leaveNeighbourhood();
  }

  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Show the selected box and its neighbours alone, "
                        "round it (N; double-click a box)");

  if (!m_graph)
    return;

  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%zu boxes, %zu arrows%s",
                      m_boxes.size(),
                      m_routeLinks.size(),
                      m_generator.running() ? " (reading the scripts...)" : "");
}

void ArchitecturePanel::findHoveredEdges()
{
  m_hoveredEdges.clear();

  if (!m_graph || m_canvas.hoveredBox() || m_canvas.busy() ||
      !ImGui::IsItemHovered())
    return;

  const board::View& view = m_canvas.view();
  const ImVec2 mouse = view.toCanvas(ImGui::GetIO().MousePos);
  const float spread = spreadAt(view.zoom);
  float best = LINK_HIT / view.zoom;

  // Mostly bundled: the trunk under the mouse, with every arrow it carries.
  if (spread < 0.5f)
  {
    const arch::RoutePiece* found = nullptr;

    for (const arch::RoutePiece& piece : m_routes.pieces)
    {
      ImRect bounds(ImMin(piece.a, piece.b), ImMax(piece.a, piece.b));
      bounds.Expand(best);

      if (!bounds.Contains(mouse))
        continue;

      const std::array<ImVec2, 2> ends = {piece.a, piece.b};
      const float distance = board::pathDistance(mouse, ends);

      if (distance < best)
      {
        best = distance;
        found = &piece;
      }
    }

    if (!found)
      return;

    for (uint32_t k = 0; k < found->linkCount; ++k)
      m_hoveredEdges.push_back(
          m_linkEdges[m_routes.pieceLinks[found->firstLink + k]]);

    std::ranges::sort(m_hoveredEdges);
    return;
  }

  // Spread: the one arrow nearest the mouse.
  for (uint32_t link = 0; link < m_routeLinks.size(); ++link)
  {
    ImRect bounds = m_routes.bounds[link];
    bounds.Expand(best);

    if (!bounds.Contains(mouse))
      continue;

    arch::routePath(m_routes, link, spread, m_path);
    const float distance = board::pathDistance(mouse, m_path);

    if (distance < best)
    {
      best = distance;
      m_hoveredEdges.assign(1, m_linkEdges[link]);
    }
  }
}

void ArchitecturePanel::focusFile(const std::filesystem::path& file)
{
  const std::optional<uint32_t> node =
      arch::nodeOfFile(*m_graph, file.generic_string());

  if (!node)
    return;

  // Already the one selected (its code opened from the board): the view
  // stays where it is.
  const std::vector<board::BoxId>& selection = m_canvas.selection();

  if (selection.size() == 1 && selection.front() == *node + 1)
    return;

  if (m_neighbourhoodMode)
  {
    showNeighbourhood(*node, true);
    return;
  }

  selectNode(*node, false);
  frameAround(*node);
}

void ArchitecturePanel::frameAround(uint32_t node)
{
  const arch::Graph& graph = *m_graph;
  const std::vector<ImVec2>& at = positions();
  ImRect bounds(at[node], at[node] + m_sizes[node]);

  for (const uint32_t touching : graph.edgesOf(node))
  {
    const arch::Edge& edge = graph.edges[touching];
    const uint32_t other = edge.from == node ? edge.to : edge.from;

    if (m_filter.shows(graph, edge) && m_shown[other])
      bounds.Add(ImRect(at[other], at[other] + m_sizes[other]));
  }

  m_canvas.frameRect(bounds);
}

void ArchitecturePanel::frameNode(uint32_t node)
{
  const ImVec2 min = positions()[node];
  m_canvas.frameRect(
      ImRect(min - FRAME_ROOM, min + m_sizes[node] + FRAME_ROOM));
}

void ArchitecturePanel::handleKeys()
{
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F))
    m_searchFocus = true;

  if (!m_graph || ImGui::GetIO().WantTextInput)
    return;

  if (ImGui::Shortcut(ImGuiKey_F))
    m_canvas.frame();

  if (ImGui::Shortcut(ImGuiKey_N))
  {
    if (m_neighbourhoodMode)
      leaveNeighbourhood();
    else if (m_canvas.selection().size() == 1)
      showNeighbourhood(
          static_cast<uint32_t>(m_canvas.selection().front() - 1), true);
  }

  if (ImGui::Shortcut(ImGuiKey_Escape))
  {
    if (m_neighbourhoodMode)
      leaveNeighbourhood();
    else
      m_canvas.clearSelection();
  }
}

void ArchitecturePanel::layoutAll(bool keepSaved)
{
  if (!keepSaved)
    m_saved.clear();

  arch::layoutGraph(*m_graph, m_sizes, m_saved, m_positions);
  bool added = false;

  for (std::size_t i = 0; i < m_positions.size(); ++i)
    added |= m_saved.try_emplace(m_graph->nodes[i].id, m_positions[i]).second;

  if (added || !keepSaved)
    savePositions();

  rebuildView();
}

void ArchitecturePanel::leaveNeighbourhood()
{
  m_neighbourhoodMode = false;
  rebuildView();

  if (m_canvas.selection().size() == 1)
    frameNode(static_cast<uint32_t>(m_canvas.selection().front() - 1));
  else
    m_canvas.frame();
}

void ArchitecturePanel::openSite(EditorContext& context,
                                 uint32_t file,
                                 int line) const
{
  context.openText = m_projectRoot / m_graph->files[file];
  context.openTextLine = line;
}

void ArchitecturePanel::pickHit(uint32_t node)
{
  selectNode(node, true);
  m_query = m_graph->nodes[node].name;
  arch::search(*m_graph, m_query, SEARCH_LIMIT, m_hits);
  m_hitChoice = 0;
  m_picked = node;
  m_resultsHovered = false;

  // A text field shows a changed value only once it isn't being typed in.
  ImGui::ClearActiveID();
  m_searchActive = false;
}

void ArchitecturePanel::rebuildView()
{
  if (!m_graph)
    return;

  const arch::Graph& graph = *m_graph;
  const std::vector<ImVec2>& at = positions();
  const std::size_t count = graph.nodes.size();
  m_shown.resize(count);
  m_boxOf.assign(count, UINT32_MAX);
  m_boxes.clear();
  m_rects.clear();

  for (std::size_t i = 0; i < count; ++i)
  {
    m_shown[i] = m_filter.shows(graph.nodes[i]) &&
                 (!m_neighbourhoodMode || m_inNeighbourhood[i]);

    if (!m_shown[i])
      continue;

    m_boxOf[i] = static_cast<uint32_t>(m_boxes.size());
    m_boxes.push_back({i + 1, at[i], at[i] + m_sizes[i]});
    m_rects.emplace_back(at[i], at[i] + m_sizes[i]);
  }

  // Each shown edge routed between the shown boxes, round them all.
  m_routeLinks.clear();
  m_linkEdges.clear();

  for (std::size_t i = 0; i < graph.edges.size(); ++i)
  {
    const arch::Edge& edge = graph.edges[i];

    if (!m_filter.shows(graph, edge) || !m_shown[edge.from] ||
        !m_shown[edge.to])
      continue;

    m_routeLinks.push_back({m_boxOf[edge.from], m_boxOf[edge.to]});
    m_linkEdges.push_back(static_cast<uint32_t>(i));
  }

  m_router.route(m_rects, m_routeLinks, m_routes);

  // A trunk is its arrows' kind's colour if most are of one kind, and
  // thicker the more it carries.
  const std::size_t pieces = m_routes.pieces.size();
  m_pieceColors.resize(pieces);
  m_pieceWidths.resize(pieces);

  for (std::size_t p = 0; p < pieces; ++p)
  {
    const arch::RoutePiece& piece = m_routes.pieces[p];
    std::array<uint32_t, arch::EDGE_KINDS> kinds{};

    for (uint32_t k = 0; k < piece.linkCount; ++k)
    {
      const uint32_t link = m_routes.pieceLinks[piece.firstLink + k];
      ++kinds[static_cast<std::size_t>(graph.edges[m_linkEdges[link]].kind)];
    }

    const auto most = std::ranges::max_element(kinds);
    m_pieceColors[p] =
        *most * 2 > piece.linkCount
            ? EDGE_COLORS[static_cast<std::size_t>(most - kinds.begin())]
            : TRUNK_COLOR;
    m_pieceWidths[p] =
        1.5f + std::log2(static_cast<float>(piece.linkCount)) * 1.2f;
  }

  m_canvas.deselectIf([&](board::BoxId id)
                      { return id == 0 || id > count || !m_shown[id - 1]; });
  m_litStale = true;
}

void ArchitecturePanel::savePositions()
{
  if (!m_positionsFile.empty())
    arch::writePositions(m_positionsFile, m_saved);
}

void ArchitecturePanel::selectNode(uint32_t node, bool frame)
{
  const arch::Node& shown = m_graph->nodes[node];
  bool& kindOn = m_filter.nodeKinds[static_cast<std::size_t>(shown.kind)];
  const bool wasOff = !kindOn;
  kindOn = true;

  if (m_neighbourhoodMode && (!m_inNeighbourhood[node] || wasOff))
  {
    showNeighbourhood(node, true);
    return;
  }

  if (wasOff)
    rebuildView();

  m_canvas.select(node + 1);

  if (frame)
    frameNode(node);
}

void ArchitecturePanel::showNeighbourhood(uint32_t centre, bool frame)
{
  const arch::Graph& graph = *m_graph;
  m_filter.nodeKinds[static_cast<std::size_t>(graph.nodes[centre].kind)] = true;
  arch::neighbourhood(graph, m_filter, centre, m_neighbourhood);
  m_inNeighbourhood.assign(graph.nodes.size(), 0);
  m_inNeighbourhood[centre] = 1;

  for (const uint32_t node : m_neighbourhood.incoming)
    m_inNeighbourhood[node] = 1;

  for (const uint32_t node : m_neighbourhood.outgoing)
    m_inNeighbourhood[node] = 1;

  arch::layoutNeighbourhood(m_neighbourhood, m_sizes, m_neighbourhoodPositions);
  m_neighbourhoodMode = true;
  m_canvas.select(centre + 1);
  rebuildView();

  if (frame)
    m_canvas.frameRect(boundsOf(m_boxes));
}

void ArchitecturePanel::startRun(const EditorContext& context)
{
  m_runAt.reset();

  if (!context.project)
    return;

  const std::string& lsp = m_environment.lsp();
  std::error_code error;

  if (lsp.empty() ||
      (m_environment.runner() == BuildEnvironment::Runner::Native &&
       !std::filesystem::exists(lsp, error)))
  {
    m_generator.stop();
    m_error = "sfs-lsp wasn't found" + (lsp.empty() ? "" : " at " + lsp) +
              ". Build it (the sfs-lsp target) to see the architecture.";
    return;
  }

  const Project& project = *context.project;
  m_generator.start(m_environment.command(
      {lsp,
       "--graph",
       "--progress",
       "--root",
       m_environment.toBuildPath(project.root()),
       m_environment.toBuildPath(project.scriptsFolder())}));
}

void ArchitecturePanel::updateLit()
{
  m_focus.clear();

  if (const board::BoxId hovered = m_canvas.hoveredBox())
    m_focus.push_back(static_cast<uint32_t>(hovered - 1));
  else
    for (const board::BoxId id : m_canvas.selection())
      m_focus.push_back(static_cast<uint32_t>(id - 1));

  if (!m_litStale && m_focus == m_litFocus && m_hoveredEdges == m_litEdges)
    return;

  arch::light(*m_graph, m_filter, m_focus, m_hoveredEdges, m_lit);
  m_litFocus = m_focus;
  m_litEdges = m_hoveredEdges;
  m_litStale = false;
}

namespace
{

// The rectangle round every box.
ImRect boundsOf(std::span<const board::Box> boxes)
{
  ImRect bounds(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));

  for (const board::Box& box : boxes)
    bounds.Add(ImRect(box.min, box.max));

  return bounds;
}

// A legend entry: a checkbox, the kind's colour (a swatch, or a line for an
// arrow), its name and how many there are. The entry is the last item, for a
// right click.
bool drawKindToggle(const char* label,
                    ImU32 color,
                    bool line,
                    std::size_t count,
                    bool& on)
{
  ImGui::BeginGroup();
  const bool changed = ImGui::Checkbox("##on", &on);
  ImGui::SameLine();
  const float height = ImGui::GetFrameHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const ImVec2 swatch(height * 0.9f, height);
  ImDrawList* draw = ImGui::GetWindowDrawList();

  if (line)
    draw->AddLine(ImVec2(at.x, at.y + height * 0.5f),
                  ImVec2(at.x + swatch.x, at.y + height * 0.5f),
                  color,
                  3.0f);
  else
    draw->AddRectFilled(ImVec2(at.x, at.y + height * 0.2f),
                        ImVec2(at.x + swatch.x, at.y + height * 0.8f),
                        color,
                        2.0f);

  ImGui::Dummy(swatch);
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::TextDisabled("%zu", count);
  ImGui::EndGroup();
  return changed;
}

ImU32 headerColor(const arch::Node& node)
{
  const ImU32 color = NODE_COLORS[static_cast<std::size_t>(node.kind)];

  if (!node.engine)
    return color;

  const ImVec4 own = ImGui::ColorConvertU32ToFloat4(color);
  const ImVec4 grey = ImGui::ColorConvertU32ToFloat4(ENGINE_GREY);
  return ImGui::ColorConvertFloat4ToU32(ImLerp(own, grey, 0.6f));
}

// The line under a box's title: its kind, an event's role, and whether it's
// the engine's or isn't declared in the scripts.
std::string kindLine(const arch::Node& node)
{
  std::string line(arch::nodeKindName(node.kind));

  if (!node.role.empty())
    line += " (" + node.role + ")";

  if (node.engine)
    line += ", engine";
  else if (!node.declared)
    line += ", not declared";

  return line;
}

const char* phaseLabel(std::string_view phase)
{
  if (phase == "scan")
    return "Listing";

  if (phase == "parse")
    return "Parsing";

  if (phase == "index")
    return "Indexing";

  if (phase == "edges")
    return "Linking";

  if (phase == "write")
    return "Writing";

  return "Reading";
}

// How far the arrows are spread into lanes at a zoom: 0 bundled, 1 spread.
float spreadAt(float zoom)
{
  const float t = std::clamp(
      (zoom - BUNDLED_ZOOM) / (SPREAD_ZOOM - BUNDLED_ZOOM), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

ImU32 withAlpha(ImU32 color, int alpha)
{
  return (color & ~IM_COL32_A_MASK) |
         (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

} // namespace
