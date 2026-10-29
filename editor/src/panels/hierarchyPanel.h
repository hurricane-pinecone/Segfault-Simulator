#pragma once

#include "panels/panel.h"

#include "engine/core/ecs/entity.h"
#include "imgui.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sfs::hierarchy
{
class EntityTree;
}

// Shows the scene's entities as a tree of parents and children; click to
// select. Drag an entity onto another to make it a child (it keeps its place
// in the world), or onto the empty space below the list to make it a root.
// Right-click empty space (or the + button) to create entities, right-click an
// entity to create a child, unparent or delete it (with its children).
// The filter box narrows the tree to a flat list of matching names; picking an
// entity elsewhere (in the viewport) opens its parents and scrolls to it.
class HierarchyPanel : public Panel
{
public:
  HierarchyPanel() : Panel("Hierarchy") {}

protected:
  void drawContents(EditorContext& context) override;

private:
  // An edit chosen while the tree is drawn, applied after it, so the tree
  // isn't changed while it's being walked.
  struct PendingEdit
  {
    enum class Kind
    {
      None,
      Reparent,   // `entity` under `parent` (invalid: a root)
      Delete,     // `entity` and its children
      MakePrefab, // `entity`'s branch into a new prefab
      Place,      // an instance of `prefab` under `parent` (invalid: a root)
    };
    Kind kind = Kind::None;
    sfs::Entity entity;
    sfs::Entity parent;
    std::string prefab;
  };

  void drawTree(EditorContext& context,
                const sfs::hierarchy::EntityTree& tree,
                const sfs::Entity& entity);

  // One row: the entity's tree node, plus its drag and drop and context menu.
  // @return true if it's open with children to draw (the caller pops it).
  // The row's focus button (frames the entity in the viewport).
  void startRename(const sfs::Entity& entity);
  void finishRename(EditorContext& context, const sfs::Entity& entity);
  void drawRenameBox(EditorContext& context, const sfs::Entity& entity, ImVec2 rowMin, ImVec2 rowMax);
  void drawFocusButton(EditorContext& context, const sfs::Entity& entity);
  bool drawRow(EditorContext& context, const sfs::Entity& entity, bool hasChildren);

  void drawDropBelow(EditorContext& context);
  void drawCreateMenu(EditorContext& context, const sfs::Entity& parent = {});
  void applyPendingEdit(EditorContext& context);

  std::string m_filter;
  uint32_t m_shownSelection = UINT32_MAX; // the selection the list last scrolled to
  const void* m_shownScene = nullptr;     // the scene it's for
  std::unordered_map<const void*, uint32_t> m_shownSelections; // other scenes
  bool m_scrollToSelection = false;
  std::unordered_set<uint32_t> m_reveal; // ids to open this frame (the selection's parents)
  PendingEdit m_pending;
  // Renaming a row in place (F2, or Rename): its entity's id, the name as
  // typed, and whether the box still needs the keyboard.
  uint32_t m_renaming = UINT32_MAX;
  std::string m_renameText;
  bool m_renameFocus = false;
  // A click on the selected row's name (renames it unless a double-click
  // follows): the row's entity id and when.
  uint32_t m_slowClick = UINT32_MAX;
  // The rows in the order drawn: this frame's, and last frame's (what a
  // Shift-click selects a range of).
  std::vector<sfs::Entity> m_rows;
  std::vector<sfs::Entity> m_lastRows;
  double m_slowClickAt = 0.0;
};
