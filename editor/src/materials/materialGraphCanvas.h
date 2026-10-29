#pragma once

#include "engine/core/rendering/3d/materialGraph.h"
#include "engine/core/rendering/3d/materialGraphCompiler.h"

#include "imgui.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sfs
{
class AssetDatabase;
}

// The Material Editor's canvas: draws a material graph's nodes and links, and
// edits them with the mouse.
//
//   - Drag a node to move it (and the rest of the selection with it); click to
//     select, Ctrl+click to add or remove one, drag on empty space to select
//     with a box.
//   - Drag from a pin to another to link them; drop on empty space to add a
//     node there, linked. Dragging a linked input picks its link up
//     (to drop on another input, or on nothing to remove it); Alt+click
//     a pin to cut its links.
//   - Right-click (or Space) for the Add Node menu; right-click a node for its
//     own. Middle- or right-drag pans; the wheel zooms.
//   - A texture dropped from the Assets panel becomes a Texture node.
//   - Values are edited on the nodes: each unlinked input's own value, and a
//     Colour or Number node's. (Zoomed far out they're too small to use, and
//     show as text; the sidebar edits them too.)
//
// Keys go to the panel, which calls the commands below.
class MaterialGraphCanvas
{
public:
  using Graph = sfs::materialGraph::Graph;
  using NodeId = sfs::materialGraph::NodeId;

  /**
   * Draw into the rest of the window, editing `graph`. `compiled` is the graph
   * as last compiled: its dynamic types colour the pins, its errors mark
   * nodes. `assets` names textures (and takes them dropped in).
   */
  void draw(Graph& graph,
            const sfs::materialGraph::CompiledGraph& compiled,
            const sfs::AssetDatabase* assets);

  /**
   * A gesture is under way (dragging nodes, a link or a box): the graph is
   * mid-edit, so the panel waits before recording an undo step.
   */
  bool busy() const { return m_gesture != Gesture::None; }

  const std::set<NodeId>& selection() const { return m_selection; }
  void select(NodeId node) { m_selection = {node}; }

  /** Fit the view to every node (or, if any, the selected ones) next draw. */
  void frame() { m_frameRequested = true; }

  /** The Add Node menu, where the mouse is. */
  void openAddMenu() { m_openAddMenu = true; }

  // Commands, for the panel's keys and menus. The Material Output isn't
  // deleted or duplicated: a graph has the one. Copied with the rest (Ctrl+A,
  // Ctrl+C), it pastes into another graph's: its links and values join that
  // graph's output.
  void deleteSelection(Graph& graph);
  void copySelection(const Graph& graph) const;
  void paste(Graph& graph);
  void duplicateSelection(Graph& graph);
  void selectAll(const Graph& graph);

private:
  enum class Gesture
  {
    None,
    Pan,
    MoveNodes,
    BoxSelect,
    Link,
  };

  // A pin: a node's input or output.
  struct Pin
  {
    NodeId node = 0;
    uint32_t index = 0;
    bool output = false;

    bool operator==(const Pin&) const = default;
  };

  // Where a node and its pins are, in canvas units.
  struct Layout
  {
    ImVec2 min, max;
    std::vector<ImVec2> inputs, outputs; // pin centres
  };

  Layout layout(const sfs::materialGraph::Node& node,
                const sfs::materialGraph::CompiledGraph& compiled,
                const sfs::AssetDatabase* assets) const;
  ImVec2 toScreen(ImVec2 canvas) const;
  ImVec2 toCanvas(ImVec2 screen) const;
  std::optional<Pin> pinAt(ImVec2 screen) const;
  NodeId nodeAt(ImVec2 screen) const;
  void fit(ImVec2 size);

  void handleMouse(Graph& graph);
  // Lay every node out (selected ones last, on top), for hit tests and drawing.
  void layoutNodes(const Graph& graph,
                   const sfs::materialGraph::CompiledGraph& compiled,
                   const sfs::AssetDatabase* assets);
  // Where an added or pasted node goes: the mouse, if it's over the canvas,
  // else the middle of the view (canvas units).
  ImVec2 placement() const;
  void finishLink(Graph& graph, ImVec2 mouse);
  void drawGrid(ImDrawList* draw, ImVec2 size) const;
  void drawLinks(ImDrawList* draw,
                 const Graph& graph,
                 const sfs::materialGraph::CompiledGraph& compiled) const;
  void drawNode(ImDrawList* draw,
                const sfs::materialGraph::Node& node,
                const Graph& graph,
                const sfs::materialGraph::CompiledGraph& compiled,
                const sfs::AssetDatabase* assets) const;
  // The node's value fields, on it (at zooms where they're usable).
  void drawFields(sfs::materialGraph::Node& node,
                  const Graph& graph,
                  const sfs::materialGraph::CompiledGraph& compiled);
  // The colour picker a colour field opened, at normal size.
  void drawColorPicker(Graph& graph);
  bool fieldsShown() const;
  void drawAddMenu(Graph& graph);
  void drawNodeMenu(Graph& graph);
  void acceptTextureDrop(Graph& graph, const sfs::AssetDatabase* assets);

  // Add a node where the Add Node menu opened, linked to the pin a link was
  // dragged from (if any) at its first fitting pin.
  void addNode(Graph& graph, const sfs::materialGraph::NodeType& type);
  // Paste nodes saved by copySelection(), with their top left at `at`.
  void pasteJson(Graph& graph, const std::string& text, ImVec2 at);

  // The view: canvas point (0, 0) sits at m_origin + m_pan on screen.
  ImVec2 m_origin{0.0f, 0.0f};
  ImVec2 m_size{0.0f, 0.0f}; // as last drawn
  ImVec2 m_pan{0.0f, 0.0f};
  float m_zoom = 1.0f;
  bool m_frameRequested = true;

  std::set<NodeId> m_selection;
  // Pin centres as last drawn, for hit tests (canvas units).
  std::vector<std::pair<Pin, ImVec2>> m_pins;
  std::vector<std::pair<NodeId, Layout>> m_layouts; // in drawing order

  Gesture m_gesture = Gesture::None;
  ImVec2 m_pressedAt{0.0f, 0.0f}; // where the gesture began (screen)
  std::vector<std::pair<NodeId, ImVec2>>
      m_moveStart; // MoveNodes: positions then
  Pin m_linkFrom;  // Link: the pin it started at
  bool m_linkPickedUp =
      false; // Link: taken off an input (dropping it on nothing unlinks)

  bool m_openAddMenu = false;
  ImVec2 m_addAt{0.0f, 0.0f};   // where an added node goes (canvas units)
  std::optional<Pin> m_addLink; // the pin a new node links to
  std::string m_addFilter;
  bool m_focusFilter = false;
  NodeId m_menuNode = 0; // the node a node menu is for

  // The colour a field's picker edits: a node's input, or (-1) its own value.
  NodeId m_pickerNode = 0;
  int m_pickerInput = -1;
  bool m_openPicker = false;
};
