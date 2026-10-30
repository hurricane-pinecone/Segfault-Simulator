#pragma once

#include "history/historySteps.h"

#include "nlohmann/json.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

struct EditorContext;

// Undo/redo for scene edits, recorded as before/after scene snapshots (the
// serializer round-trips a scene exactly, so any change -- a field drag, a new
// entity, a removed component -- undoes the same way).
//
// An edit is bracketed by beginChange() (captures "before", once) and
// endChange() (captures "after"; records the pair if anything changed), so a
// drag spanning many frames is one step. Undo/redo restore a snapshot but keep
// the camera where it is, and bring back the selection the step started (undo)
// or ended (redo) with. The steps are kept within a step count and a memory
// budget (see HistorySteps): with a huge scene, fewer of them.
class EditorHistory
{
public:
  explicit EditorHistory(EditorContext& context) : m_context(context) {}

  /** Start an edit, if one isn't already in progress. */
  void beginChange();

  /** Finish the edit in progress, recording it if the scene changed. */
  void endChange();

  /** Run `action` as one edit. */
  template <typename Action>
  void change(Action&& action)
  {
    beginChange();
    action();
    endChange();
  }

  /** Whether a change has begun (beginChange) and not yet ended. */
  bool changing() const { return m_pendingBefore.has_value(); }

  /**
   * Fold what's changed since into the last step (its "after"): for edits
   * that follow on from one just recorded, as one undo step (an Inspector
   * edit copied to every selected entity).
   */
  void amendLast();

  /** Counts edits, undos and redos: unchanged means the scene is as it was. */
  uint64_t revision() const { return m_revision; }

  bool canUndo() const { return m_steps.canUndo(); }
  bool canRedo() const { return m_steps.canRedo(); }
  void undo();
  void redo();

  /** Forget everything (a different scene was opened). */
  void clear();

  /**
   * Change every recorded scene the same way (e.g. a field renamed in the
   * game's code), so undoing past the change still restores what was there.
   */
  void rewrite(const std::function<void(nlohmann::json&)>& edit);

  /**
   * Stop recording (Play mode: changes there are thrown away on Stop), or
   * resume. Disabling drops any edit in progress.
   */
  void setEnabled(bool enabled);

private:
  // A selected entity by its position in save order (parents first), or none.
  // A restore recreates entities in that order, so the position names the
  // same entity afterwards; its id may differ (ids follow creation order,
  // which reparenting doesn't change until the scene is reloaded).
  using Selection = HistorySteps::Selection;

  Selection currentSelection() const;

  // Restore a snapshot and selection, keeping the camera.
  void apply(const nlohmann::json& snapshot, Selection selection);

  EditorContext& m_context;
  HistorySteps m_steps;
  std::optional<nlohmann::json> m_pendingBefore;
  Selection m_pendingSelection;
  uint64_t m_revision = 0;
  bool m_enabled = true;
};
