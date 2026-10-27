#include "nvim/nvimScreen.h"

#include <algorithm>

using msgpack::Array;
using msgpack::Value;

namespace
{
std::optional<uint32_t> color(const Value& value)
{
  // -1 (or absent) means "use the default".
  if (!value.isInteger() || value.asInt() < 0)
    return std::nullopt;
  return static_cast<uint32_t>(value.asInt());
}
} // namespace

void NvimScreen::applyRedraw(const Array& events)
{
  // Each event is [name, args...] where every args is one call's arguments.
  for (const Value& event : events)
  {
    const Array& parts = event.asArray();
    if (parts.empty())
      continue;
    const std::string& name = parts.front().asString();
    for (std::size_t i = 1; i < parts.size(); ++i)
      apply(name, parts[i].asArray());
  }
}

void NvimScreen::apply(const std::string& name, const Array& args)
{
  const auto arg = [&](std::size_t i) -> const Value&
  {
    static const Value nil;
    return i < args.size() ? args[i] : nil;
  };

  if (name == "grid_line")
    gridLine(args);
  else if (name == "grid_cursor_goto")
  {
    m_back.cursorRow = static_cast<int>(arg(1).asInt());
    m_back.cursorColumn = static_cast<int>(arg(2).asInt());
  }
  else if (name == "grid_scroll")
    gridScroll(args);
  else if (name == "grid_clear")
    std::fill(m_back.cells.begin(), m_back.cells.end(), Cell{});
  else if (name == "grid_resize")
  {
    const int columns = static_cast<int>(arg(1).asInt());
    const int rows = static_cast<int>(arg(2).asInt());
    Grid resized;
    resized.columns = columns;
    resized.rows = rows;
    resized.cells.assign(static_cast<std::size_t>(columns * rows), Cell{});
    // Keep what fits; Neovim redraws the rest.
    for (int r = 0; r < std::min(rows, m_back.rows); ++r)
      for (int c = 0; c < std::min(columns, m_back.columns); ++c)
        resized.at(r, c) = m_back.at(r, c);
    resized.cursorRow = std::min(m_back.cursorRow, std::max(rows - 1, 0));
    resized.cursorColumn = std::min(m_back.cursorColumn, std::max(columns - 1, 0));
    m_back = std::move(resized);
  }
  else if (name == "flush")
    m_front = m_back;
  else if (name == "default_colors_set")
  {
    if (const auto fg = color(arg(0)))
      m_defaultForeground = *fg;
    if (const auto bg = color(arg(1)))
      m_defaultBackground = *bg;
    if (const auto sp = color(arg(2)))
      m_defaultSpecial = *sp;
  }
  else if (name == "hl_attr_define")
  {
    const Value& rgb = arg(1);
    Highlight highlight;
    highlight.foreground = color(rgb["foreground"]);
    highlight.background = color(rgb["background"]);
    highlight.special = color(rgb["special"]);
    highlight.bold = rgb["bold"].asBool();
    highlight.italic = rgb["italic"].asBool();
    highlight.underline = rgb["underline"].asBool();
    highlight.undercurl = rgb["undercurl"].asBool();
    highlight.strikethrough = rgb["strikethrough"].asBool();
    highlight.reverse = rgb["reverse"].asBool();
    m_highlights[static_cast<int>(arg(0).asInt())] = highlight;
  }
  else if (name == "mode_info_set")
  {
    m_modes.clear();
    for (const Value& info : arg(1).asArray())
    {
      ModeInfo mode;
      const std::string& shape = info["cursor_shape"].asString();
      if (shape == "vertical")
        mode.shape = CursorShape::Vertical;
      else if (shape == "horizontal")
        mode.shape = CursorShape::Horizontal;
      mode.percentage = static_cast<int>(info["cell_percentage"].asInt(100));
      m_modes.push_back(mode);
    }
  }
  else if (name == "mode_change")
  {
    const auto index = static_cast<std::size_t>(arg(1).asInt());
    if (index < m_modes.size())
    {
      m_cursorShape = m_modes[index].shape;
      m_cursorPercentage = m_modes[index].percentage;
    }
  }
  else if (name == "set_title")
    m_title = arg(0).asString();
  // Everything else (options, busy, mouse on/off, ...) doesn't change what's drawn.
}

void NvimScreen::gridLine(const Array& args)
{
  if (args.size() < 4)
    return;
  const int row = static_cast<int>(args[1].asInt());
  int column = static_cast<int>(args[2].asInt());
  if (row < 0 || row >= m_back.rows)
    return;

  // Cells are [text, hl_id?, repeat?]; an omitted hl_id repeats the previous.
  int highlight = 0;
  for (const Value& entry : args[3].asArray())
  {
    const Array& cell = entry.asArray();
    if (cell.empty())
      continue;
    if (cell.size() >= 2)
      highlight = static_cast<int>(cell[1].asInt());
    const int repeat = cell.size() >= 3 ? static_cast<int>(cell[2].asInt()) : 1;

    for (int i = 0; i < repeat && column < m_back.columns; ++i, ++column)
      m_back.at(row, column) = Cell{cell[0].asString(), highlight};
  }
}

void NvimScreen::gridScroll(const Array& args)
{
  if (args.size() < 6)
    return;
  const int top = static_cast<int>(args[1].asInt());
  const int bottom = static_cast<int>(args[2].asInt()); // exclusive
  const int left = static_cast<int>(args[3].asInt());
  const int right = static_cast<int>(args[4].asInt()); // exclusive
  const int rows = static_cast<int>(args[5].asInt());   // >0: content moves up

  const auto copyRow = [&](int to, int from)
  {
    for (int c = left; c < right; ++c)
      m_back.at(to, c) = m_back.at(from, c);
  };

  // The rows scrolled in are left as they were: Neovim redraws them next.
  if (rows > 0)
    for (int r = top; r < bottom - rows; ++r)
      copyRow(r, r + rows);
  else if (rows < 0)
    for (int r = bottom - 1; r >= top - rows; --r)
      copyRow(r, r + rows);
}

const NvimScreen::Cell& NvimScreen::cell(int row, int column) const
{
  static const Cell blank;
  if (row < 0 || row >= m_front.rows || column < 0 || column >= m_front.columns)
    return blank;
  return m_front.cells[static_cast<std::size_t>(row * m_front.columns + column)];
}

const NvimScreen::Highlight& NvimScreen::highlight(int id) const
{
  static const Highlight none;
  const auto it = m_highlights.find(id);
  return it != m_highlights.end() ? it->second : none;
}

NvimScreen::Colors NvimScreen::colorsOf(const Cell& cell) const
{
  const Highlight& hl = highlight(cell.highlight);
  Colors colors{hl.foreground.value_or(m_defaultForeground),
                hl.background.value_or(m_defaultBackground),
                hl.special.value_or(m_defaultSpecial)};
  if (hl.reverse)
    std::swap(colors.foreground, colors.background);
  return colors;
}
