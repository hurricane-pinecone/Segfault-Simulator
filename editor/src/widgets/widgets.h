#pragma once

#include "history/editorHistory.h"

#include "imgui.h"

#include <string>

// Small ImGui helpers shared by the editor's panels.
namespace widgets
{

/** ImGui::InputText over a std::string, growing it as the user types. */
bool inputText(const char* label, std::string& value, int flags = 0);

// ImGui's colour pickers, kept to 0..1 (ImGui only limits dragging, so a typed
// 1000 in a 0..255 field would otherwise stick).
bool colorEdit3(const char* label, float* rgb);
bool colorEdit4(const char* label, float* rgba);

/**
 * A search box `width` wide (-1: the rest of the line): `hint` shows faintly
 * while it's empty, and an x at its end clears it (as Esc does while typing).
 *
 * @return true when the text changed
 */
bool searchInput(const char* id, std::string& value, const char* hint, float width = -1.0f);

/**
 * Square icon buttons a frame high: a pencil (edit), and a crosshair
 * (target: find or frame something). @return true when clicked
 */
bool editButton(const char* id);
bool targetButton(const char* id);

/** The icons themselves, `size` across, for drawing into other widgets. */
void drawPencil(ImDrawList* draw, ImVec2 centre, float size, ImU32 color);
void drawTarget(ImDrawList* draw, ImVec2 centre, float size, ImU32 color);

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
