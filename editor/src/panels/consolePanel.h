#pragma once

#include "panels/panel.h"

#include "engine/core/logger/logger.h"

#include <deque>
#include <mutex>
#include <string>

// Shows the engine log. Subscribes to the logger for its lifetime and keeps the
// most recent messages; errors are highlighted.
class ConsolePanel : public Panel
{
public:
  ConsolePanel();
  ~ConsolePanel() override;

protected:
  void drawContents(EditorContext& context) override;

private:
  struct Entry
  {
    sfs::Logger::Level level;
    std::string text;
  };

  static constexpr std::size_t kMaxEntries = 2000;

  sfs::Logger::SinkId m_sink;
  std::mutex m_mutex; // the logger may call from any thread
  std::deque<Entry> m_entries;
  bool m_autoScroll = true;
};
