#include "panels/consolePanel.h"

#include "imgui.h"

ConsolePanel::ConsolePanel() : Panel("Console")
{
  m_sink = sfs::Logger::addSink(
      [this](sfs::Logger::Level level, const std::string& message)
      {
        const std::lock_guard lock(m_mutex);
        m_entries.push_back({level, message});
        if (m_entries.size() > kMaxEntries)
          m_entries.pop_front();
      });
}

ConsolePanel::~ConsolePanel() { sfs::Logger::removeSink(m_sink); }

void ConsolePanel::drawContents(EditorContext&)
{
  if (ImGui::Button("Clear"))
  {
    const std::lock_guard lock(m_mutex);
    m_entries.clear();
  }
  ImGui::SameLine();
  ImGui::Checkbox("Auto-scroll", &m_autoScroll);
  ImGui::Separator();

  ImGui::BeginChild("ConsoleLog", ImVec2(0, 0), ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar);
  {
    const std::lock_guard lock(m_mutex);
    for (const Entry& entry : m_entries)
    {
      const bool error = entry.level == sfs::Logger::Level::ERROR;
      const bool warning = entry.level == sfs::Logger::Level::WARN;
      if (error || warning)
        ImGui::PushStyleColor(ImGuiCol_Text,
                              error ? ImVec4(1.0f, 0.45f, 0.40f, 1.0f) : ImVec4(1.0f, 0.80f, 0.35f, 1.0f));
      ImGui::TextUnformatted(entry.text.c_str());
      if (error || warning)
        ImGui::PopStyleColor();
    }
  }

  // Follow new output only while already at the bottom, so scrolling up to
  // read isn't yanked back down.
  if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
    ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
}
