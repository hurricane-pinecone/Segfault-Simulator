#pragma once

#include "nvim/msgpack.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Neovim's screen as its UI protocol (ext_linegrid) describes it: a grid of
// cells, each a character and a highlight id, plus highlight definitions,
// default colours, the cursor and its shape. Built from `redraw` events;
// double-buffered so the panel only ever draws complete frames (a frame
// becomes visible at each `flush`).
class NvimScreen
{
public:
  struct Cell
  {
    std::string text = " "; // one screen cell's UTF-8 text ("" for the right half of a wide char)
    int highlight = 0;
  };

  struct Highlight
  {
    std::optional<uint32_t> foreground; // 0xRRGGBB
    std::optional<uint32_t> background;
    std::optional<uint32_t> special;    // underline/undercurl colour
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool undercurl = false;
    bool strikethrough = false;
    bool reverse = false;
  };

  enum class CursorShape
  {
    Block,
    Vertical,   // insert mode bar
    Horizontal, // replace mode underline
  };

  struct Colors
  {
    uint32_t foreground = 0;
    uint32_t background = 0;
    uint32_t special = 0;
  };

  /** Apply the params of one `redraw` notification (a batch of events). */
  void applyRedraw(const msgpack::Array& events);

  int columns() const { return m_front.columns; }
  int rows() const { return m_front.rows; }
  const Cell& cell(int row, int column) const;
  const Highlight& highlight(int id) const;

  /** A cell's resolved colours: defaults filled in, reverse applied. */
  Colors colorsOf(const Cell& cell) const;
  uint32_t defaultBackground() const { return m_defaultBackground; }

  int cursorRow() const { return m_front.cursorRow; }
  int cursorColumn() const { return m_front.cursorColumn; }
  CursorShape cursorShape() const { return m_cursorShape; }
  /** How much of the cell a bar/underline cursor covers (0..100). */
  int cursorPercentage() const { return m_cursorPercentage; }

  /** Window title Neovim asks for (set_title), e.g. the current file. */
  const std::string& title() const { return m_title; }

private:
  struct Grid
  {
    int columns = 0;
    int rows = 0;
    int cursorRow = 0;
    int cursorColumn = 0;
    std::vector<Cell> cells;

    Cell& at(int row, int column) { return cells[static_cast<std::size_t>(row * columns + column)]; }
  };

  struct ModeInfo
  {
    CursorShape shape = CursorShape::Block;
    int percentage = 100;
  };

  void apply(const std::string& name, const msgpack::Array& args);
  void gridLine(const msgpack::Array& args);
  void gridScroll(const msgpack::Array& args);

  Grid m_back;  // being built from events
  Grid m_front; // last flushed frame, what's drawn

  std::unordered_map<int, Highlight> m_highlights;
  uint32_t m_defaultForeground = 0xd0d0d0;
  uint32_t m_defaultBackground = 0x1c1c1c;
  uint32_t m_defaultSpecial = 0xff0000;

  std::vector<ModeInfo> m_modes;
  CursorShape m_cursorShape = CursorShape::Block;
  int m_cursorPercentage = 100;
  std::string m_title;
};
