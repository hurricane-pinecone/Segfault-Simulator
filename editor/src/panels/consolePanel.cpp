#include "panels/consolePanel.h"

#include "engine/core/logger/logger.h"
#include "panels/panel.h"
#include "widgets/widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace
{
constexpr float MIN_DETAIL_HEIGHT = 60.0f;
constexpr float SPLITTER_HEIGHT = 6.0f;

ImU32 levelColor(sfs::Logger::Level level);
} // namespace

ConsolePanel::ConsolePanel() : Panel("Console")
{
  m_sink = sfs::Logger::addSink(
      [this](sfs::Logger::Level level, const std::string& message)
      {
        const std::lock_guard lock(m_mutex);

        // A message of several lines (a build's output) gets a row per line,
        // so every row is one line high and only the rows in view are drawn.
        std::string_view rest = message;

        while (true)
        {
          const std::size_t end = rest.find('\n');
          std::string_view line = rest.substr(0, end);

          if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

          m_lines.push_back({level, std::string(line)});

          if (end == std::string_view::npos || end + 1 == rest.size())
            break;

          rest.remove_prefix(end + 1);
        }

        while (m_lines.size() > kMaxLines)
        {
          setSelected(m_lines.front(), false);
          m_lines.pop_front();
          ++m_firstId;
        }
      });
}

ConsolePanel::~ConsolePanel() { sfs::Logger::removeSink(m_sink); }

void ConsolePanel::drawContents(EditorContext&)
{
  const std::lock_guard lock(m_mutex);

  if (ImGui::Button("Clear"))
    clear();

  ImGui::SameLine();
  ImGui::Checkbox("Auto-scroll", &m_autoScroll);
  ImGui::Separator();
  handleKeys();

  // The detail strip shows under the list while lines are selected, as high
  // as the splitter between them leaves it.
  const bool detail = m_selectedCount > 0;
  float listHeight = 0.0f;

  if (detail)
  {
    const float available = ImGui::GetContentRegionAvail().y;
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    m_detailHeight = std::clamp(m_detailHeight,
                                MIN_DETAIL_HEIGHT,
                                std::max(MIN_DETAIL_HEIGHT, available * 0.8f));
    listHeight =
        std::max(available - m_detailHeight - SPLITTER_HEIGHT - 2.0f * spacing,
                 ImGui::GetFrameHeight());
  }

  ImGui::BeginChild("Lines",
                    ImVec2(0.0f, listHeight),
                    ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar);
  drawLines();
  drawMenu();

  // Follow new output only while already at the bottom with nothing selected,
  // so scrolling up to read, or picking lines to copy, isn't yanked back down.
  if (m_autoScroll && m_selectedCount == 0 &&
      ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
    ImGui::SetScrollHereY(1.0f);

  ImGui::EndChild();

  // Kept out for this frame if the selection was cleared while drawing it.
  if (!detail || m_selectedCount == 0)
    return;

  m_detailHeight -= widgets::splitter(
      "##splitter", ImVec2(ImGui::GetContentRegionAvail().x, SPLITTER_HEIGHT));
  drawDetail();
}

bool ConsolePanel::appendText(std::string& out,
                              bool selectedOnly,
                              std::size_t limit) const
{
  bool first = true;

  for (const Line& line : m_lines)
  {
    if (selectedOnly && !line.selected)
      continue;

    if (!first && out.size() + 1 + line.text.size() > limit)
      return false;

    if (!first)
      out += '\n';

    out += line.text;
    first = false;
  }

  return true;
}

void ConsolePanel::applySelection(const ImGuiMultiSelectIO& io)
{
  const std::int64_t count = static_cast<std::int64_t>(m_lines.size());

  for (const ImGuiSelectionRequest& request : io.Requests)
  {
    if (request.Type == ImGuiSelectionRequestType_SetAll)
    {
      setAllSelected(request.Selected);
      continue;
    }

    if (request.Type != ImGuiSelectionRequestType_SetRange)
      continue;

    // Ids of lines dropped since don't count.
    const std::int64_t first =
        std::min(request.RangeFirstItem, request.RangeLastItem) - m_firstId;
    const std::int64_t last =
        std::max(request.RangeFirstItem, request.RangeLastItem) - m_firstId;

    for (std::int64_t i = std::max<std::int64_t>(first, 0);
         i <= std::min(last, count - 1);
         ++i)
      setSelected(m_lines[static_cast<std::size_t>(i)], request.Selected);
  }
}

void ConsolePanel::clear()
{
  m_firstId += static_cast<std::int64_t>(m_lines.size());
  m_lines.clear();
  m_selectedCount = 0;
  m_detailStale = true;
  m_widestLine = 0.0f;
}

void ConsolePanel::copy(bool selectedOnly) const
{
  std::string text;
  appendText(text, selectedOnly);
  ImGui::SetClipboardText(text.c_str());
}

void ConsolePanel::drawDetail()
{
  if (std::exchange(m_detailStale, false))
  {
    m_detail.clear();
    m_detailComplete = appendText(m_detail, true, kMaxDetailBytes);
  }

  if (m_selectedCount == 1)
    ImGui::TextDisabled("1 line selected");
  else if (m_detailComplete)
    ImGui::TextDisabled("%zu lines selected", m_selectedCount);
  else
    ImGui::TextDisabled("%zu lines selected, the first of them shown here "
                        "(Ctrl+C copies them all)",
                        m_selectedCount);

  // Read-only: ImGui only reads the buffer, from the string's own memory.
  ImGui::InputTextMultiline(
      "##detail",
      m_detail.data(),
      m_detail.size() + 1,
      ImVec2(-FLT_MIN, -FLT_MIN),
      ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
}

void ConsolePanel::drawLines()
{
  // A drag from a line selects the lines it passes (box select), from any
  // line, selected or not: rows aren't dragged anywhere.
  const ImGuiMultiSelectFlags flags = ImGuiMultiSelectFlags_NoSelectAll |
                                      ImGuiMultiSelectFlags_ClearOnEscape |
                                      ImGuiMultiSelectFlags_ClearOnClickVoid |
                                      ImGuiMultiSelectFlags_BoxSelect1d |
                                      ImGuiMultiSelectFlags_SelectOnClickAlways;
  const int count = static_cast<int>(m_lines.size());
  const ImGuiMultiSelectIO* begin =
      ImGui::BeginMultiSelect(flags, static_cast<int>(m_selectedCount), count);
  applySelection(*begin);

  // The line Shift+click and Shift+arrows extend from must be submitted even
  // when out of view. Without one (an id of -1), or with one dropped since,
  // it's out of range.
  ImGuiListClipper clipper;
  clipper.Begin(count);
  const std::int64_t anchor = begin->RangeSrcItem - m_firstId;

  if (anchor >= 0 && anchor < count)
    clipper.IncludeItemByIndex(static_cast<int>(anchor));

  const float width = ImGui::GetContentRegionAvail().x;
  ImDrawList* draw = ImGui::GetWindowDrawList();

  while (clipper.Step())
  {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
    {
      const Line& line = m_lines[static_cast<std::size_t>(i)];
      const char* text = line.text.data();
      const char* textEnd = text + line.text.size();

      // As wide as the widest line seen, for the horizontal scrollbar.
      m_widestLine =
          std::max(m_widestLine, ImGui::CalcTextSize(text, textEnd).x);

      // The text is drawn over a label-less row: a label would hash the line
      // every frame and hide what follows a "##" in it.
      const ImVec2 position = ImGui::GetCursorScreenPos();
      const std::int64_t id = m_firstId + i;
      ImGui::PushID(static_cast<int>(id));
      ImGui::SetNextItemSelectionUserData(id);
      ImGui::Selectable("##line",
                        line.selected,
                        ImGuiSelectableFlags_None,
                        ImVec2(std::max(width, m_widestLine), 0.0f));
      ImGui::PopID();
      draw->AddText(position, levelColor(line.level), text, textEnd);
    }
  }

  applySelection(*ImGui::EndMultiSelect());
}

void ConsolePanel::drawMenu()
{
  if (!ImGui::BeginPopupContextWindow("Menu"))
    return;

  if (ImGui::MenuItem("Copy", "Ctrl+C", false, m_selectedCount > 0))
    copy(true);

  if (ImGui::MenuItem("Copy All", nullptr, false, !m_lines.empty()))
    copy(false);

  if (ImGui::MenuItem("Select All", "Ctrl+A", false, !m_lines.empty()))
    setAllSelected(true);

  ImGui::Separator();

  if (ImGui::MenuItem("Clear"))
    clear();

  ImGui::EndPopup();
}

void ConsolePanel::handleKeys()
{
  // Routed to the focused window: these act only while the Console (or a
  // window inside it) has the focus, and the detail's text box takes its own
  // Ctrl+A and Ctrl+C while it's being used.
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A))
    setAllSelected(true);

  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C) && m_selectedCount > 0)
    copy(true);
}

void ConsolePanel::setAllSelected(bool selected)
{
  for (Line& line : m_lines)
    setSelected(line, selected);
}

void ConsolePanel::setSelected(Line& line, bool selected)
{
  if (line.selected == selected)
    return;

  line.selected = selected;
  m_detailStale = true;

  if (selected)
    ++m_selectedCount;
  else
    --m_selectedCount;
}

namespace
{
ImU32 levelColor(sfs::Logger::Level level)
{
  if (level == sfs::Logger::Level::ERROR)
    return ImGui::GetColorU32(ImVec4(1.0f, 0.45f, 0.40f, 1.0f));

  if (level == sfs::Logger::Level::WARN)
    return ImGui::GetColorU32(ImVec4(1.0f, 0.80f, 0.35f, 1.0f));

  return ImGui::GetColorU32(ImGuiCol_Text);
}
} // namespace
