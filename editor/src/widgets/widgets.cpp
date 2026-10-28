#include "widgets/widgets.h"

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
