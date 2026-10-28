#include "widgets/widgets.h"

#include "widgets/theme.h"

#include <algorithm>

#include "imgui.h"

namespace widgets
{

namespace
{
bool clamped(bool changed, float* values, int count)
{
  if (changed)
    for (int i = 0; i < count; ++i)
      values[i] = std::clamp(values[i], 0.0f, 1.0f);
  return changed;
}
} // namespace

bool colorEdit3(const char* label, float* rgb) { return clamped(ImGui::ColorEdit3(label, rgb), rgb, 3); }

bool colorEdit4(const char* label, float* rgba) { return clamped(ImGui::ColorEdit4(label, rgba), rgba, 4); }

namespace
{
// ImGui asks to grow the buffer as the text outgrows it.
int resizeCallback(ImGuiInputTextCallbackData* data)
{
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
  {
    auto* value = static_cast<std::string*>(data->UserData);
    value->resize(static_cast<std::size_t>(data->BufTextLen));
    data->Buf = value->data();
  }
  return 0;
}
} // namespace

bool inputText(const char* label, std::string& value, int flags)
{
  return ImGui::InputText(label,
                          value.data(),
                          value.capacity() + 1,
                          flags | ImGuiInputTextFlags_CallbackResize,
                          resizeCallback,
                          &value);
}

namespace
{
// A square icon button a frame high, drawn like the hierarchy's: the icon in
// faint ink, accent on hover over a soft circle.
template <typename Draw>
bool iconButton(const char* id, Draw&& drawIcon)
{
  const float side = ImGui::GetFrameHeight();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(side, side));
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 centre(min.x + side * 0.5f, min.y + side * 0.5f);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  if (hovered)
    draw->AddCircleFilled(centre, side * 0.4f, theme::kAccentSoft);
  drawIcon(draw, centre, side * 0.8f, hovered ? theme::kAccent : ImGui::GetColorU32(ImGuiCol_TextDisabled));
  return clicked;
}
} // namespace

void drawTarget(ImDrawList* draw, ImVec2 centre, float size, ImU32 color)
{
  draw->AddCircle(centre, size * 0.28f, color, 0, 1.5f);
  for (const ImVec2 direction : {ImVec2(1, 0), ImVec2(-1, 0), ImVec2(0, 1), ImVec2(0, -1)})
    draw->AddLine(ImVec2(centre.x + direction.x * size * 0.18f, centre.y + direction.y * size * 0.18f),
                  ImVec2(centre.x + direction.x * size * 0.45f, centre.y + direction.y * size * 0.45f),
                  color,
                  1.5f);
}

void drawPencil(ImDrawList* draw, ImVec2 centre, float size, ImU32 color)
{
  // Lying from its tip at bottom-left to its end at top-right.
  const float reach = size * 0.32f, half = size * 0.08f;
  const auto along = [&](float t, float across)
  {
    const float d = -reach + 2.0f * reach * t; // tip (t = 0) to end (t = 1)
    return ImVec2(centre.x + d - across, centre.y - d - across);
  };
  draw->AddQuadFilled(along(0.32f, half), along(1.0f, half), along(1.0f, -half), along(0.32f, -half), color);
  draw->AddTriangleFilled(along(0.32f, half), along(0.32f, -half), along(0.0f, 0.0f), color);
}

bool editButton(const char* id) { return iconButton(id, drawPencil); }

bool targetButton(const char* id) { return iconButton(id, drawTarget); }

bool searchInput(const char* id, std::string& value, const char* hint, float width)
{
  ImGui::PushID(id);
  // The clear button sits on top of the box, so the box lets it take clicks.
  ImGui::SetNextItemAllowOverlap();
  ImGui::SetNextItemWidth(width);
  bool changed = inputText("##search", value, ImGuiInputTextFlags_EscapeClearsAll);
  const bool active = ImGui::IsItemActive();
  const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
  const ImGuiStyle& style = ImGui::GetStyle();
  ImDrawList* draw = ImGui::GetWindowDrawList();

  if (value.empty() && !active)
    draw->AddText(ImVec2(min.x + style.FramePadding.x, min.y + style.FramePadding.y),
                  ImGui::GetColorU32(ImGuiCol_TextDisabled),
                  hint);
  if (!value.empty())
  {
    const float size = max.y - min.y;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(max.x - size, min.y));
    if (ImGui::InvisibleButton("clear", ImVec2(size, size)))
    {
      value.clear();
      changed = true;
    }
    const bool hovered = ImGui::IsItemHovered();
    ImGui::SetItemTooltip("Clear");
    ImGui::SetCursorScreenPos(cursor);

    const ImVec2 centre(max.x - size * 0.5f, min.y + size * 0.5f);
    if (hovered)
      draw->AddCircleFilled(centre, size * 0.32f, ImGui::GetColorU32(ImGuiCol_ButtonHovered));
    const float arm = size * 0.14f;
    const ImU32 color = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    draw->AddLine(ImVec2(centre.x - arm, centre.y - arm), ImVec2(centre.x + arm, centre.y + arm), color, 1.5f);
    draw->AddLine(ImVec2(centre.x - arm, centre.y + arm), ImVec2(centre.x + arm, centre.y - arm), color, 1.5f);
  }
  ImGui::PopID();
  return changed;
}

} // namespace widgets
