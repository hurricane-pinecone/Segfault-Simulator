#include "history/editorHistory.h"

#include "editorContext.h"
#include "scene/editorScene.h"
#include "scene/sceneDocument.h"

#include "engine/runtime/rendering/renderSystem.h"

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

  const nlohmann::json after = m_context.document->snapshot();
  if (after != *m_pendingBefore)
    m_steps.push(*m_pendingBefore, after, m_pendingSelection, currentSelection());
  m_pendingBefore.reset();
}

void EditorHistory::pushAssetEdit(std::function<void()> undo, std::function<void()> redo, std::size_t bytes)
{
  if (!m_enabled)
    return;
  m_steps.pushAssetEdit(
      std::make_shared<const HistorySteps::AssetEdit>(
          HistorySteps::AssetEdit{std::move(undo), std::move(redo), bytes}),
      currentSelection());
}

void EditorHistory::amendLast()
{
  if (m_pendingBefore || !m_steps.canUndo() || !m_context.document)
    return;
  m_steps.amendLast(m_context.document->snapshot(), currentSelection());
}

void EditorHistory::undo()
{
  if (!canUndo() || m_pendingBefore)
    return;
  if (const std::optional<HistorySteps::Restore> restore = m_steps.undo())
  {
    if (restore->asset)
      restore->asset->undo();
    else
      apply(restore->snapshot, restore->selection);
  }
}

void EditorHistory::redo()
{
  if (!canRedo() || m_pendingBefore)
    return;
  if (const std::optional<HistorySteps::Restore> restore = m_steps.redo())
  {
    if (restore->asset)
      restore->asset->redo();
    else
      apply(restore->snapshot, restore->selection);
  }
}

void EditorHistory::rewrite(const std::function<void(nlohmann::json&)>& edit)
{
  m_steps.rewrite(edit);
}

void EditorHistory::clear()
{
  m_steps.clear();
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
  return m_context.scene->saveOrderOf(m_context.selection);
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
    m_context.selection = m_context.scene->atSaveOrder(*selection);
}
