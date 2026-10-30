#pragma once

#include "nlohmann/json.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

// The undo and redo stacks of an EditorHistory: before/after scene snapshots,
// kept as compact JSON text (a fraction of a parsed document's memory), the
// "before" of an edit sharing the text of the "after" of the one before it
// when they're the same scene (the usual case), and capped both by number of
// steps and by the bytes they hold, so undoing a huge scene's edits can't eat
// all the memory there is. The oldest steps go first; the latest is always
// kept, however big.
class HistorySteps
{
public:
  // A selected entity by its position in save order, or none (see
  // EditorHistory).
  using Selection = std::optional<std::size_t>;

  static constexpr std::size_t kDefaultMaxSteps = 200;
  static constexpr std::size_t kDefaultMaxBytes = std::size_t(256) << 20;

  explicit HistorySteps(std::size_t maxSteps = kDefaultMaxSteps, std::size_t maxBytes = kDefaultMaxBytes)
      : m_maxSteps(maxSteps), m_maxBytes(maxBytes)
  {
  }

  /** Record an edit (dropping what could be redone, and the oldest steps past the limits). */
  void
  push(const nlohmann::json& before, const nlohmann::json& after, Selection selectionBefore, Selection selectionAfter);

  /** Replace the last step's "after" (an edit folded into it). */
  void amendLast(const nlohmann::json& after, Selection selectionAfter);

  /** A scene to restore, and the selection to restore with it. */
  struct Restore
  {
    nlohmann::json snapshot;
    Selection selection;
  };

  bool canUndo() const { return !m_undo.empty(); }
  bool canRedo() const { return !m_redo.empty(); }
  /** Step back: the scene as it was before the last edit. Nothing if there's none. */
  std::optional<Restore> undo();
  /** Step forward again: the scene as it was after the edit last undone. */
  std::optional<Restore> redo();

  void clear();

  /** Change every recorded scene the same way (see EditorHistory::rewrite). */
  void rewrite(const std::function<void(nlohmann::json&)>& edit);

  std::size_t undoSteps() const { return m_undo.size(); }
  std::size_t redoSteps() const { return m_redo.size(); }
  /** The snapshots' text, counted per step (shared text counted each time: an upper bound). */
  std::size_t bytes() const { return m_bytes; }

private:
  using Text = std::shared_ptr<const std::string>;

  struct Step
  {
    Text before;
    Text after;
    Selection selectionBefore;
    Selection selectionAfter;

    std::size_t bytes() const { return before->size() + after->size(); }
  };

  static Text text(const nlohmann::json& snapshot);
  // Drop the oldest steps while over the limits (keeping the latest).
  void trim();

  std::size_t m_maxSteps;
  std::size_t m_maxBytes;
  std::deque<Step> m_undo;
  std::deque<Step> m_redo;
  std::size_t m_bytes = 0; // of m_undo and m_redo
};
