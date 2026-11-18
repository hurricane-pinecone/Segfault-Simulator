#pragma once

#include "engine/core/logger/logger.h"
#include "panels/panel.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

// Shows the engine log, a row per line, keeping the most recent lines; errors
// and warnings are coloured. Subscribes to the logger for its lifetime. Rows
// are selected as a list's are (click, Shift/Ctrl+click, drag, Ctrl+A) and
// copied with Ctrl+C; the selection's text is shown in full under the list,
// where any part of it can be selected and copied.
class ConsolePanel : public Panel
{
public:
  ConsolePanel();
  ~ConsolePanel() override;

protected:
  void drawContents(EditorContext& context) override;

private:
  struct Line
  {
    sfs::Logger::Level level;
    std::string text;
    bool selected = false;
  };

  // Append the lines (only the selected ones if `selectedOnly`) to `out`, one
  // per line, stopping before one that would take it past `limit` bytes (the
  // first always fits). Returns whether every one was appended.
  bool appendText(std::string& out,
                  bool selectedOnly,
                  std::size_t limit = SIZE_MAX) const;
  void applySelection(const ImGuiMultiSelectIO& io);
  void clear();
  void copy(bool selectedOnly) const;
  void drawDetail();
  void drawLines();
  void drawMenu();
  void handleKeys();
  void setAllSelected(bool selected);
  void setSelected(Line& line, bool selected);

  // The detail shows at most this much of a large selection; copying takes it
  // all.
  static constexpr std::size_t kMaxDetailBytes = 64 * 1024;
  // Lines kept at most, the oldest dropped first.
  static constexpr std::size_t kMaxLines = 20000;

  bool m_autoScroll = true;
  std::string m_detail;
  bool m_detailComplete = true;
  float m_detailHeight = 120.0f;
  bool m_detailStale = true;
  // The selection id of m_lines.front(): a line keeps its id while older ones
  // are dropped, so the selection and its Shift+click anchor stay on it.
  std::int64_t m_firstId = 0;
  std::deque<Line> m_lines;
  std::mutex m_mutex; // the logger may call from any thread
  std::size_t m_selectedCount = 0;
  sfs::Logger::SinkId m_sink;
  float m_widestLine = 0.0f;
};
