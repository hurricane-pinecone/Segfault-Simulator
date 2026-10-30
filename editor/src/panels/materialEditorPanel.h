#pragma once

#include "materials/materialGraphCanvas.h"
#include "materials/materialPreview.h"
#include "panels/panel.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/rendering/material.h"
#include "engine/core/rendering/materialGraphCompiler.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace sfs
{
class IRenderBackend;
}

// A node-graph material open for editing, in its own tab: the graph's canvas,
// and beside it a preview and the selected node's values.
//
// Edits show in the open scene straight away (its objects with this material
// redraw), but the file changes only when saved (Ctrl+S); closing with unsaved
// changes asks first, and discarding puts the scene back as the file has it.
// Undo (Ctrl+Z) steps back through this material's edits, a whole drag at a
// time.
class MaterialEditorPanel : public Panel
{
public:
  MaterialEditorPanel(sfs::IRenderBackend& backend,
                      const sfs::AssetRecord& record);

  const sfs::AssetId& material() const { return m_material; }

  /** Read the material's file. False (logged) if it isn't a graph material. */
  bool load(EditorContext& context);

  /** Changed since it was last saved (or opened). */
  bool unsaved() const { return m_asset != m_saved; }

  /** Write the file (and redraw the scene and the Assets panel's preview). */
  bool save(EditorContext& context);

  /** Its tab was closed, its changes saved or discarded: remove the panel. */
  bool finished() const { return m_finished; }

  /** Put the scene back to the material as saved (closing without saving). */
  void revertScene(EditorContext& context) const;

  /** Open as a tab beside this window (another material's). */
  void dockWith(std::string title) { m_dockWith = std::move(title); }

  /**
   * Open below this window (the scene's viewport), splitting its space: the
   * scene stays in view, showing edits as they're made.
   */
  void dockBelow(std::string title) { m_dockBelow = std::move(title); }

  /** Whose the Edit menu is: this panel's while it was the last one focused. */
  bool ownsEdit() const { return m_ownsEdit; }

  bool canUndo() const { return !m_undo.empty(); }
  bool canRedo() const { return !m_redo.empty(); }
  void undo();
  void redo();
  void duplicateSelection() { m_canvas.duplicateSelection(*m_asset.graph); }
  void deleteSelection() { m_canvas.deleteSelection(*m_asset.graph); }

  void update(EditorContext& context) override;

protected:
  void drawContents(EditorContext& context) override;
  void beforeBegin() override;
  int windowFlags() const override;

private:
  void handleKeys(EditorContext& context);
  void drawToolbar(EditorContext& context);
  void drawSidebar(EditorContext& context);
  void drawNodeValues(EditorContext& context, sfs::materialGraph::Node& node);
  void drawPreview();
  void drawCloseDialog(EditorContext& context);
  // After any edit: compile, show it in the scene, start a new preview.
  void applyChanges(EditorContext& context);
  // Record an undo step once an edit is finished (not mid-drag).
  void recordUndo();

  sfs::AssetId m_material;
  std::string m_name; // its file's name, for messages

  sfs::MaterialAsset m_asset;        // as being edited (has a graph)
  sfs::MaterialAsset m_saved;        // as in the file
  sfs::MaterialAsset m_applied;      // as last compiled and shown
  const void* m_appliedTo = nullptr; // the scene it was shown in
  sfs::MaterialAsset m_committed;    // as of the last undo step
  sfs::materialGraph::CompiledGraph m_compiled;
  std::vector<sfs::MaterialAsset> m_undo, m_redo;

  MaterialGraphCanvas m_canvas;
  std::string m_dockWith;
  std::string m_dockBelow;
  bool m_ownsEdit = false;
  bool m_askClose = false;
  bool m_finished = false;
  float m_sidebarWidth = 300.0f;

  MaterialPreview m_preview;
  std::chrono::steady_clock::time_point
      m_opened; // the preview's Time counts from here
};
