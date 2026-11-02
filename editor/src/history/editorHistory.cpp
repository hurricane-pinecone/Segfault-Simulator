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

  const nlohmann::json after = m_context.document->snapshot();
  if (after != *m_pendingBefore)
  {
    m_steps.push(*m_pendingBefore, after, m_pendingSelection, currentSelection());
    ++m_revision;
  }
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
  ++m_revision;
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
  ++m_revision;
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
  ++m_revision;
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
