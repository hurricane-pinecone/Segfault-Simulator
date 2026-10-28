#include "../../engine/tests/testHarness.h"

#include "nvim/nvimScreen.h"

#include <string>

using msgpack::Array;
using msgpack::Map;
using msgpack::Value;

namespace
{
Value str(const char* text) { return Value(std::string(text)); }

// One redraw event: [name, args...].
Value event(const char* name, std::initializer_list<Array> calls)
{
  Array parts{str(name)};
  for (const Array& call : calls)
    parts.emplace_back(call);
  return Value(parts);
}

std::string rowText(const NvimScreen& screen, int row)
{
  std::string text;
  for (int c = 0; c < screen.columns(); ++c)
    text += screen.cell(row, c).text;
  return text;
}

NvimScreen screenWith(int columns, int rows)
{
  NvimScreen screen;
  screen.applyRedraw({event("grid_resize", {{Value(1), Value(columns), Value(rows)}}),
                      event("flush", {{}})});
  return screen;
}
} // namespace

int main()
{
  TEST("grid_line should fill cells, repeating and inheriting highlights")
  {
    NvimScreen screen = screenWith(10, 2);
    screen.applyRedraw({event("grid_line",
                              {{Value(1), Value(0), Value(0),
                                Value(Array{Value(Array{str("a"), Value(3)}),
                                            Value(Array{str("b")}), // inherits hl 3
                                            Value(Array{str("-"), Value(0), Value(3)})})}}),
                        event("flush", {{}})});

    CHECK(rowText(screen, 0) == std::string("ab---     "));
    CHECK(screen.cell(0, 1).highlight == 3);
    CHECK(screen.cell(0, 2).highlight == 0);
  }

  TEST("nothing should show until flush")
  {
    NvimScreen screen = screenWith(4, 1);
    screen.applyRedraw({event("grid_line",
                              {{Value(1), Value(0), Value(0),
                                Value(Array{Value(Array{str("x"), Value(0), Value(4)})})}})});
    CHECK(rowText(screen, 0) == std::string("    "));
    screen.applyRedraw({event("flush", {{}})});
    CHECK(rowText(screen, 0) == std::string("xxxx"));
  }

  TEST("grid_scroll should move rows up and down within the region")
  {
    NvimScreen screen = screenWith(3, 4);
    Array lines;
    for (int r = 0; r < 4; ++r)
      screen.applyRedraw({event("grid_line",
                                {{Value(1), Value(r), Value(0),
                                  Value(Array{Value(Array{Value(std::string(1, char('0' + r))),
                                                          Value(0), Value(3)})})}})});
    screen.applyRedraw({event("grid_scroll",
                              {{Value(1), Value(0), Value(4), Value(0), Value(3), Value(1)}}),
                        event("flush", {{}})});
    CHECK(rowText(screen, 0) == std::string("111"));
    CHECK(rowText(screen, 2) == std::string("333"));

    screen.applyRedraw({event("grid_scroll",
                              {{Value(1), Value(0), Value(4), Value(0), Value(3), Value(-2)}}),
                        event("flush", {{}})});
    CHECK(rowText(screen, 2) == std::string("111"));
    CHECK(rowText(screen, 3) == std::string("222"));
  }

  TEST("highlights and default colours should resolve, with reverse")
  {
    NvimScreen screen = screenWith(2, 1);
    Map rgb;
    rgb.emplace_back(str("foreground"), Value(0xff0000));
    rgb.emplace_back(str("reverse"), Value(true));
    screen.applyRedraw(
        {event("default_colors_set",
               {{Value(0x111111), Value(0x222222), Value(0x333333), Value(-1), Value(-1)}}),
         event("hl_attr_define", {{Value(7), Value(rgb), Value(Map{}), Value(Array{})}}),
         event("grid_line",
               {{Value(1), Value(0), Value(0), Value(Array{Value(Array{str("x"), Value(7)}),
                                                         Value(Array{str("y"), Value(0)})})}}),
         event("flush", {{}})});

    const NvimScreen::Colors reversed = screen.colorsOf(screen.cell(0, 0));
    CHECK(reversed.foreground == 0x222222u); // default background, swapped in
    CHECK(reversed.background == 0xff0000u);
    const NvimScreen::Colors plain = screen.colorsOf(screen.cell(0, 1));
    CHECK(plain.foreground == 0x111111u);
    CHECK(plain.background == 0x222222u);
  }

  TEST("mode changes should set the cursor shape")
  {
    NvimScreen screen = screenWith(2, 1);
    Map normal;
    normal.emplace_back(str("cursor_shape"), str("block"));
    Map insert;
    insert.emplace_back(str("cursor_shape"), str("vertical"));
    insert.emplace_back(str("cell_percentage"), Value(25));
    screen.applyRedraw(
        {event("mode_info_set", {{Value(true), Value(Array{Value(normal), Value(insert)})}}),
         event("mode_change", {{str("insert"), Value(1)}})});
    CHECK(screen.cursorShape() == NvimScreen::CursorShape::Vertical);
    CHECK(screen.cursorPercentage() == 25);
  }

  TEST("resizing should keep what fits and clamp the cursor")
  {
    NvimScreen screen = screenWith(4, 2);
    screen.applyRedraw({event("grid_line",
                              {{Value(1), Value(0), Value(0),
                                Value(Array{Value(Array{str("ab")}), Value(Array{str("c")})})}}),
                        event("grid_cursor_goto", {{Value(1), Value(1), Value(3)}}),
                        event("grid_resize", {{Value(1), Value(2), Value(1)}}),
                        event("flush", {{}})});
    CHECK(screen.columns() == 2);
    CHECK(screen.rows() == 1);
    CHECK(screen.cell(0, 1).text == std::string("c"));
    CHECK(screen.cursorRow() == 0);
    CHECK(screen.cursorColumn() == 1);
  }

  TEST("lines and cursors outside the grid should be ignored")
  {
    NvimScreen screen = screenWith(2, 1);
    screen.applyRedraw({event("grid_line",
                              {{Value(1), Value(5), Value(0), Value(Array{Value(Array{str("x")})})}}),
                        event("flush", {{}})});
    CHECK(rowText(screen, 0) == std::string("  "));
    CHECK(screen.cell(9, 9).text == std::string(" "));
  }

  return testing::report("nvimScreenTests");
}
