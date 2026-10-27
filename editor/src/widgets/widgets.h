#pragma once

#include "history/editorHistory.h"

#include "imgui.h"

#include <string>

// Small ImGui helpers shared by the editor's panels.
namespace widgets
{

/** ImGui::InputText over a std::string, growing it as the user types. */
bool inputText(const char* label, std::string& value, int flags = 0);

/**
 * Run an editing widget on a copy of `value`, recording the edit for undo.
 * The scene is snapshotted when the interaction starts and before the copy is
 * written back, so even a widget that changes the value on its first frame
 * (a slider click, a checkbox) records the right "before"; a drag across many
 * frames is one undo step.
 *
 *   widgets::undoable(history, transform.position,
 *                     [](glm::vec3& v) { return ImGui::DragFloat3("Position", &v.x); });
 */
template <typename T, typename Widget>
bool undoable(EditorHistory* history, T& value, Widget&& widget)
{
  T copy = value;
  const bool changed = widget(copy);

  if (history && (ImGui::IsItemActivated() || changed))
    history->beginChange();
  if (changed)
    value = copy;
  if (history && (ImGui::IsItemDeactivated() || (changed && !ImGui::IsItemActive())))
    history->endChange();

  return changed;
}

} // namespace widgets
