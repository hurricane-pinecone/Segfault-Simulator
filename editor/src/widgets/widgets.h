#pragma once

#include "history/editorHistory.h"

#include "imgui.h"

#include <string>

// Small ImGui helpers shared by the editor's panels.
namespace widgets
{

/**
 * A labelled row, as the Inspector lays its fields out: the label on the
 * left, in a column about 40% of the row (clipped, the whole of it in a
 * tooltip, when too long), and its widget on the right. Call it, then draw
 * the widget with a hidden label ("##value"): the next item fills from the
 * label column to the row's right edge or, with `width` > 0 (a checkbox's,
 * say), is that wide flush against the right edge.
 *
 *   widgets::propertyLabel("Casts Shadow", ImGui::GetFrameHeight());
 *   ImGui::Checkbox("##value", &castsShadow);
 */
void propertyLabel(const char* label, float width = 0.0f);

/** propertyLabel() then a checkbox flush right. @return true when toggled */
bool propertyCheckbox(const char* label, bool* value);

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
bool searchInput(const char* id,
                 std::string& value,
                 const char* hint,
                 float width = -1.0f);

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
 * The title of the tab showing where `window` is docked (empty if it isn't
 * docked, or not drawn yet).
 */
std::string shownTab(const char* window);

/**
 * Bring a docked window's tab to the front of its node without focusing it
 * (whatever has the keyboard keeps it).
 */
void showTab(const std::string& window);

/**
 * Keep `owns` saying whether the current window, or a window inside it, was
 * the last one focused (menus, popups and tooltips don't count): whose the
 * Edit menu and shortcuts are. Its own window, not its dock: panels docked
 * together are told apart. Call while the window is being drawn.
 *
 * @param owns kept between frames; changed only when a window counts
 */
void trackFocus(bool& owns);

/**
 * Whether the last item was clicked: the mouse released over it, pressed on
 * it, and not dragged past the drag threshold in between (a drag from the
 * item, to drop it somewhere, is no click). Select on this, as Unity does,
 * so dragging an item doesn't select it.
 *
 * @param button the mouse button
 * @return true on the frame of such a release
 */
bool itemReleasedWithoutDrag(ImGuiMouseButton button = ImGuiMouseButton_Left);

/**
 * Run an editing widget on a copy of `value`, recording the edit for undo.
 * The scene is snapshotted when the interaction starts and before the copy is
 * written back, so even a widget that changes the value on its first frame
 * (a slider click, a checkbox) records the right "before"; a drag across many
 * frames is one undo step.
 *
 *   widgets::undoable(history, transform.position,
 *                     [](glm::vec3& v) { return ImGui::DragFloat3("Position",
 * &v.x); });
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
  if (history &&
      (ImGui::IsItemDeactivated() || (changed && !ImGui::IsItemActive())))
    history->endChange();

  return changed;
}

} // namespace widgets
