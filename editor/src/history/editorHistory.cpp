#include "history/editorHistory.h"

#include "editorContext.h"
#include "scene/editorScene.h"
#include "scene/sceneDocument.h"

#include "engine/core/hierarchy/hierarchy.h"
#include "engine/runtime/rendering/renderSystem.h"

#include <algorithm>

void EditorHistory::beginChange()
{
  if (!m_enabled || m_pendingBefore || !m_context.document)
    return;
  m_pendingBefore = m_context.document->snapshot();
  m_pendingSelection = currentSelection();
}

void EditorHistory::endChange()
{
  if (!m_pendingBefore || !m_context.document)
    return;

  nlohmann::json after = m_context.document->snapshot();
  if (after != *m_pendingBefore)
  {
    m_undo.push_back(
        {std::move(*m_pendingBefore), std::move(after), m_pendingSelection, currentSelection()});
    if (m_undo.size() > kMaxSteps)
      m_undo.pop_front();
    m_redo.clear();
    ++m_revision;
  }
  m_pendingBefore.reset();
}

void EditorHistory::amendLast()
{
  if (m_pendingBefore || m_undo.empty() || !m_context.document)
    return;
  m_undo.back().after = m_context.document->snapshot();
  m_undo.back().selectionAfter = currentSelection();
}

void EditorHistory::undo()
{
  if (!canUndo() || m_pendingBefore)
    return;
  Step step = std::move(m_undo.back());
  m_undo.pop_back();
  apply(step.before, step.selectionBefore);
  m_redo.push_back(std::move(step));
  ++m_revision;
}

void EditorHistory::redo()
{
  if (!canRedo() || m_pendingBefore)
    return;
  Step step = std::move(m_redo.back());
  m_redo.pop_back();
  apply(step.after, step.selectionAfter);
  m_undo.push_back(std::move(step));
  ++m_revision;
}

void EditorHistory::rewrite(const std::function<void(nlohmann::json&)>& edit)
{
  for (std::deque<Step>* steps : {&m_undo, &m_redo})
    for (Step& step : *steps)
    {
      edit(step.before);
      edit(step.after);
    }
}

void EditorHistory::clear()
{
  m_undo.clear();
  m_redo.clear();
  m_pendingBefore.reset();
}

void EditorHistory::setEnabled(bool enabled)
{
  m_enabled = enabled;
  if (!enabled)
    m_pendingBefore.reset();
}

EditorHistory::Selection EditorHistory::currentSelection() const
{
  if (!m_context.hasSelection() || !m_context.scene)
    return std::nullopt;
  const std::vector<sfs::Entity> order =
      sfs::hierarchy::EntityTree(m_context.scene->entities()).preOrder();
  const auto it = std::find(order.begin(), order.end(), m_context.selection);
  if (it == order.end())
    return std::nullopt;
  return static_cast<std::size_t>(it - order.begin());
}

void EditorHistory::apply(const nlohmann::json& snapshot, Selection selection)
{
  if (!m_context.document || !m_context.scene)
    return;

  // Undo is about the scene, not where you were looking at it from.
  const sfs::Camera camera = m_context.render->camera();
  m_context.document->restore(snapshot);
  m_context.render->camera() = camera;

  m_context.selection = {};
  if (selection)
  {
    const std::vector<sfs::Entity> order =
        sfs::hierarchy::EntityTree(m_context.scene->entities()).preOrder();
    if (*selection < order.size())
      m_context.selection = order[*selection];
  }
}
