#include "panels/panel.h"

#include "widgets/widgets.h"

#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName, window settings

#include <memory>
#include <span>
#include <utility>

void Panel::dockNewBesideViewport()
{
  if (std::exchange(m_dockChecked, true))
    return;

  const ImGuiWindow* viewport = ImGui::FindWindowByName("Viewport");
  const ImGuiWindowSettings* settings =
      ImGui::FindWindowSettingsByID(ImHashStr(title()));

  if (viewport && viewport->DockId && !settings)
    ImGui::SetNextWindowDockID(viewport->DockId);
}

void Panel::draw(EditorContext& context)
{
  if (!m_open)
  {
    m_editFocused = false;
    return;
  }

  beforeBegin();
  const bool visible = ImGui::Begin(m_title.c_str(), &m_open, windowFlags());

  // Kept while it's a tab behind another too: that one took the focus.
  widgets::trackFocus(m_editFocused);

  if (visible)
    drawContents(context);

  ImGui::End();
}

Panel* editFocusPanel(std::span<const std::unique_ptr<Panel>> panels)
{
  for (const std::unique_ptr<Panel>& panel : panels)
    if (panel->editFocused())
      return panel.get();

  return nullptr;
}
