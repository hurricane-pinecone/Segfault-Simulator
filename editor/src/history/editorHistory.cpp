#include "history/editorHistory.h"

#include "editorContext.h"
#include "scene/editorScene.h"
#include "scene/sceneDocument.h"

#include "engine/runtime/rendering3d/renderSystem3D.h"

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
  }
  m_pendingBefore.reset();
}

void EditorHistory::undo()
{
  if (!canUndo() || m_pendingBefore)
    return;
  Step step = std::move(m_undo.back());
  m_undo.pop_back();
  apply(step.before, step.selectionBefore);
  m_redo.push_back(std::move(step));
}

void EditorHistory::redo()
{
  if (!canRedo() || m_pendingBefore)
    return;
  Step step = std::move(m_redo.back());
  m_redo.pop_back();
  apply(step.after, step.selectionAfter);
  m_undo.push_back(std::move(step));
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
  if (!m_context.hasSelection())
    return std::nullopt;
  return m_context.selection.getId();
}

void EditorHistory::apply(const nlohmann::json& snapshot, Selection selection)
{
  if (!m_context.document || !m_context.scene)
    return;

  // Undo is about the scene, not where you were looking at it from.
  const sfs::Camera3D camera = m_context.render->camera();
  m_context.document->restore(snapshot);
  m_context.render->camera() = camera;

  m_context.selection = selection ? m_context.scene->getEntity(*selection) : sfs::Entity();
}
