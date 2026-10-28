#pragma once

#include "nlohmann/json.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
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
// or ended (redo) with.
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

  /** Counts edits, undos and redos: unchanged means the scene is as it was. */
  uint64_t revision() const { return m_revision; }

  bool canUndo() const { return !m_undo.empty(); }
  bool canRedo() const { return !m_redo.empty(); }
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
  using Selection = std::optional<std::size_t>;

  struct Step
  {
    nlohmann::json before;
    nlohmann::json after;
    Selection selectionBefore;
    Selection selectionAfter;
  };

  Selection currentSelection() const;

  // Restore a snapshot and selection, keeping the camera.
  void apply(const nlohmann::json& snapshot, Selection selection);

  static constexpr std::size_t kMaxSteps = 200;

  EditorContext& m_context;
  std::deque<Step> m_undo;
  std::deque<Step> m_redo;
  std::optional<nlohmann::json> m_pendingBefore;
  Selection m_pendingSelection;
  uint64_t m_revision = 0;
  bool m_enabled = true;
};
