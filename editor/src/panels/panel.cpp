#include "panels/panel.h"

#include "imgui.h"

void Panel::draw(EditorContext& context)
{
  if (!m_open)
    return;

  beforeBegin();
  if (ImGui::Begin(m_title, &m_open, windowFlags()))
    drawContents(context);
  ImGui::End();
}
