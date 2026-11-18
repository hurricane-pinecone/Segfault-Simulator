#include "../../engine/tests/testHarness.h"
#include "widgets/boardCanvas.h"

#include <imgui.h>
#include <imgui_internal.h> // ImRect

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using board::operator+;
using board::operator-;
using board::operator*;

namespace
{
// An ImGui with no window or renderer, driven a frame at a time.
struct Gui
{
  Gui()
  {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  }
  ~Gui() { ImGui::DestroyContext(); }

  void frame(const std::function<void()>& draw)
  {
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    draw();
    ImGui::Render();
  }
};

// A board in a 400 x 300 window at the screen's top left: with ImGui's
// default padding, canvas point (0, 0) starts at screen (8, 8).
struct Board
{
  Gui gui;
  board::Canvas canvas;
  std::vector<board::Box> boxes{
      {1, {20.0f, 20.0f}, {120.0f, 80.0f}},
      {2, {200.0f, 20.0f}, {300.0f, 80.0f}},
      {3, {60.0f, 40.0f}, {160.0f, 140.0f}}, // over box 1's corner
  };
  std::function<board::Press(ImVec2)> claimPress;
  board::Input last;
  std::vector<board::BoxMove> moved; // every move reported, in order
  board::BoxId doubleClickedBox = 0;
  bool gestureEnded = false;
  int moveEnds = 0;
  bool rightClicked = false;
  board::BoxId rightClickedBox = 0;

  void frame()
  {
    gui.frame(
        [&]
        {
          ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
          ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
          ImGui::Begin("Board",
                       nullptr,
                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoResize);

          if (canvas.begin("board"))
          {
            last = canvas.handleMouse(boxes, claimPress);
            moved.insert(moved.end(), last.moved.begin(), last.moved.end());
            gestureEnded |= last.gestureEnded;
            moveEnds += last.moveEnded ? 1 : 0;
            doubleClickedBox = last.doubleClickedBox ? last.doubleClickedBox
                                                     : doubleClickedBox;
            rightClicked |= last.rightClicked;
            rightClickedBox =
                last.rightClicked ? last.rightClickedBox : rightClickedBox;
            ImDrawList* draw = canvas.beginDraw();
            canvas.drawLink(draw, boxes[0].max, boxes[1].min, {});

            for (const board::Box& box : boxes)
              canvas.drawBox(draw, box, {.title = "Box"});

            canvas.endDraw(draw);
          }

          ImGui::End();
        });
  }

  // The screen point of a canvas point, before any pan or zoom.
  static ImVec2 screen(float x, float y) { return ImVec2(x + 8.0f, y + 8.0f); }

  void move(ImVec2 to)
  {
    ImGui::GetIO().AddMousePosEvent(to.x, to.y);
    frame();
  }

  void button(ImGuiMouseButton which, bool down)
  {
    ImGui::GetIO().AddMouseButtonEvent(which, down);
    frame();
  }

  void key(ImGuiKey which, bool down)
  {
    ImGui::GetIO().AddKeyEvent(which, down);
    frame();
  }

  void click(ImVec2 at, ImGuiMouseButton which = ImGuiMouseButton_Left)
  {
    move(at);
    button(which, true);
    button(which, false);
  }

  void drag(ImVec2 from, ImVec2 to, ImGuiMouseButton which)
  {
    move(from);
    button(which, true);
    move(from + (to - from) * 0.5f);
    move(to);
    button(which, false);
  }

  std::vector<board::BoxId> selection() const { return canvas.selection(); }
};

bool near(float a, float b, float tolerance = 1e-3f)
{
  return std::abs(a - b) <= tolerance;
}

bool near(ImVec2 a, ImVec2 b, float tolerance = 1e-3f)
{
  return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance);
}

board::View view(ImVec2 origin, ImVec2 size, ImVec2 pan, float zoom)
{
  board::View out;
  out.origin = origin;
  out.size = size;
  out.pan = pan;
  out.zoom = zoom;
  return out;
}
} // namespace

int main()
{
  TEST("a view should map screen to canvas and back")
  {
    for (const float zoom : {0.25f, 1.0f, 2.5f})
    {
      const board::View v =
          view({30.0f, 40.0f}, {600.0f, 400.0f}, {-75.0f, 12.5f}, zoom);
      const ImVec2 point(123.0f, -45.0f);
      CHECK(near(v.toCanvas(v.toScreen(point)), point));
      CHECK(near(v.toScreen(v.toCanvas(point)), point));
    }

    // Canvas (0, 0) sits at origin + pan; a canvas unit is `zoom` pixels.
    const board::View v = view({30.0f, 40.0f}, {600.0f, 400.0f}, {5, 6}, 2.0f);
    CHECK(near(v.toScreen({0.0f, 0.0f}), ImVec2(35.0f, 46.0f)));
    CHECK(near(v.toScreen({10.0f, 1.0f}), ImVec2(55.0f, 48.0f)));
  }

  TEST("zooming about a point should keep that point where it is")
  {
    board::View v = view({30.0f, 40.0f}, {600.0f, 400.0f}, {-75.0f, 12.5f}, 1);
    const ImVec2 mouse(250.0f, 170.0f);
    const ImVec2 under = v.toCanvas(mouse);

    for (const float zoom : {1.15f, 0.25f, 2.5f, 0.8f})
    {
      v.zoomAbout(mouse, zoom);
      CHECK(v.zoom == zoom);
      CHECK(near(v.toScreen(under), mouse));
    }
  }

  TEST("fitting should centre what's fitted, as large as fits, in range")
  {
    board::View v = view({10.0f, 10.0f}, {400.0f, 200.0f}, {}, 1.0f);
    const ImRect bounds({100.0f, 100.0f}, {300.0f, 200.0f});
    CHECK(v.fit(bounds, 0.0f, 0.25f, 2.5f));
    // The height decides: 200 / 100.
    CHECK(near(v.zoom, 2.0f));
    CHECK(near(v.toScreen(bounds.GetCenter()), ImVec2(210.0f, 110.0f)));

    // A margin round it, and the zoom limits.
    CHECK(v.fit(bounds, 50.0f, 0.25f, 2.5f));
    CHECK(near(v.zoom, 1.0f));
    CHECK(v.fit(bounds, 0.0f, 0.25f, 1.25f));
    CHECK(near(v.zoom, 1.25f));
    CHECK(v.fit(ImRect({0, 0}, {100000.0f, 10.0f}), 0.0f, 0.25f, 2.5f));
    CHECK(near(v.zoom, 0.25f));

    // Nothing to fit, or nowhere to fit it: unchanged.
    const board::View before = v;
    const ImRect empty({FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX});
    CHECK(!v.fit(empty, 0.0f, 0.25f, 2.5f));
    v.size = {0.0f, 0.0f};
    CHECK(!v.fit(bounds, 0.0f, 0.25f, 2.5f));
    CHECK(v.zoom == before.zoom);
    CHECK(near(v.pan, before.pan));
  }

  TEST("culling should keep what reaches the view and skip what doesn't")
  {
    // The view shows canvas 0..400 x 0..300.
    const board::View v = view({0.0f, 0.0f}, {400.0f, 300.0f}, {}, 1.0f);
    CHECK(v.shows({10.0f, 10.0f}, {50.0f, 50.0f}, 0.0f));
    CHECK(v.shows({-50.0f, -50.0f}, {10.0f, 10.0f}, 0.0f));     // straddling
    CHECK(v.shows({-500.0f, -500.0f}, {900.0f, 900.0f}, 0.0f)); // covering
    CHECK(!v.shows({500.0f, 10.0f}, {600.0f, 50.0f}, 0.0f));
    CHECK(!v.shows({10.0f, -90.0f}, {50.0f, -10.0f}, 0.0f));
    // Pins and outlines reach past a box: the pad keeps it.
    CHECK(v.shows({405.0f, 10.0f}, {500.0f, 50.0f}, 8.0f));

    using board::LinkShape;
    CHECK(v.showsLink({50.0f, 50.0f}, {100.0f, 80.0f}, LinkShape::Curved, 0));
    CHECK(
        !v.showsLink({500.0f, 50.0f}, {600.0f, 80.0f}, LinkShape::Straight, 0));
    // Both ends off either side of the view, the link across it.
    CHECK(
        v.showsLink({-50.0f, 50.0f}, {450.0f, 80.0f}, LinkShape::Straight, 0));
    // A curve bulges past its ends by its handles: from left of the view, its
    // start handle reaches into it.
    CHECK(v.showsLink({-30.0f, 50.0f}, {-200.0f, 80.0f}, LinkShape::Curved, 0));
    CHECK(!v.showsLink(
        {-30.0f, 50.0f}, {-200.0f, 80.0f}, LinkShape::Straight, 0));
  }

  TEST("the topmost box containing a point should be hit")
  {
    const std::array<board::Box, 3> boxes{{
        {1, {0.0f, 0.0f}, {100.0f, 100.0f}},
        {2, {50.0f, 50.0f}, {150.0f, 150.0f}},
        {3, {300.0f, 0.0f}, {400.0f, 100.0f}},
    }};
    CHECK(board::boxAt(boxes, {10.0f, 10.0f})->id == 1);
    CHECK(board::boxAt(boxes, {75.0f, 75.0f})->id == 2); // drawn last: on top
    CHECK(board::boxAt(boxes, {350.0f, 50.0f})->id == 3);
    CHECK(board::boxAt(boxes, {200.0f, 50.0f}) == nullptr);
    // A box holds its top left edge, not its bottom right one.
    CHECK(board::boxAt(boxes, {300.0f, 0.0f})->id == 3);
    CHECK(board::boxAt(boxes, {400.0f, 50.0f}) == nullptr);
    CHECK(board::boxAt({}, {10.0f, 10.0f}) == nullptr);
  }

  TEST("a link should be hit near its line or curve")
  {
    using board::LinkShape;
    const ImVec2 from(0.0f, 0.0f), to(200.0f, 100.0f);
    CHECK(near(board::linkDistance(
                   {100.0f, 50.0f}, from, to, LinkShape::Straight, 1.0f),
               0.0f));
    // Perpendicular to the line (direction 2, 1): (-1, 2) / sqrt 5 away.
    const ImVec2 off = ImVec2(100.0f, 50.0f) + ImVec2(-10.0f, 20.0f);
    CHECK(near(board::linkDistance(off, from, to, LinkShape::Straight, 1.0f),
               std::sqrt(500.0f)));
    // Past its end: the distance to the end.
    CHECK(near(board::linkDistance(
                   {230.0f, 140.0f}, from, to, LinkShape::Straight, 1.0f),
               50.0f));

    // An S-curve passes through the middle of its ends, and its ends.
    const std::array<ImVec2, 4> points = board::curve(from, to, 1.0f);
    CHECK(near(points[1], ImVec2(100.0f, 0.0f)));
    CHECK(near(points[2], ImVec2(100.0f, 100.0f)));
    CHECK(board::linkDistance({100.0f, 50.0f}, from, to, LinkShape::Curved, 1) <
          1.5f);
    CHECK(board::linkDistance(from, from, to, LinkShape::Curved, 1.0f) < 1.5f);
    CHECK(board::linkDistance({100.0f, 90.0f}, from, to, LinkShape::Curved, 1) >
          20.0f);
    // Ends close together keep handles 40 canvas units long, at the zoom.
    const std::array<ImVec2, 4> tight =
        board::curve({0.0f, 0.0f}, {10.0f, 50.0f}, 2.0f);
    CHECK(near(tight[1], ImVec2(80.0f, 0.0f)));
    CHECK(near(tight[2], ImVec2(-70.0f, 50.0f)));
  }

  TEST("a curve should leave and enter on the sides its look names")
  {
    using board::LinkSide;
    const ImVec2 from(200.0f, 0.0f), to(0.0f, 100.0f);

    // Leftwards: out of the left side, into the right side.
    const std::array<ImVec2, 4> back =
        board::curve(from, to, 1.0f, LinkSide::Left, LinkSide::Right);
    CHECK(near(back[1], ImVec2(100.0f, 0.0f)));
    CHECK(near(back[2], ImVec2(100.0f, 100.0f)));

    // Out of the right side and back into the right side: a bow out to the
    // right, wider the further apart the ends are.
    const ImVec2 below(200.0f, 400.0f);
    const std::array<ImVec2, 4> bow =
        board::curve(from, below, 1.0f, LinkSide::Right, LinkSide::Right);
    CHECK(near(bow[1], ImVec2(300.0f, 0.0f)));
    CHECK(near(bow[2], ImVec2(300.0f, 400.0f)));
    CHECK(board::linkDistance({275.0f, 200.0f},
                              from,
                              below,
                              board::LinkShape::Curved,
                              1.0f,
                              LinkSide::Right,
                              LinkSide::Right) < 1.5f);

    // Culling knows the bow reaches past its ends.
    const board::View narrow =
        view({250.0f, 0.0f}, {100.0f, 400.0f}, {-250.0f, 0.0f}, 1.0f);
    CHECK(!narrow.showsLink(from, below, board::LinkShape::Curved, 0.0f));
    CHECK(narrow.showsLink(from,
                           below,
                           board::LinkShape::Curved,
                           0.0f,
                           LinkSide::Right,
                           LinkSide::Right));
  }

  TEST("a double click on a box should be reported, and a move's end")
  {
    Board b;
    b.frame();
    b.click(Board::screen(250.0f, 50.0f));
    CHECK(b.doubleClickedBox == 0);
    CHECK(b.moveEnds == 1); // a click is a move that goes nowhere
    b.click(Board::screen(250.0f, 50.0f));
    CHECK(b.doubleClickedBox == 2);
    CHECK((b.selection() == std::vector<board::BoxId>{2}));

    b.drag(Board::screen(250.0f, 50.0f),
           Board::screen(260.0f, 70.0f),
           ImGuiMouseButton_Left);
    CHECK(b.moveEnds == 3);
  }

  TEST("a dragged-out rectangle should be the same either way round")
  {
    const ImRect a = board::rectBetween({100.0f, 20.0f}, {10.0f, 80.0f});
    CHECK(near(a.Min, ImVec2(10.0f, 20.0f)));
    CHECK(near(a.Max, ImVec2(100.0f, 80.0f)));
    const ImRect b = board::rectBetween({10.0f, 80.0f}, {100.0f, 20.0f});
    CHECK(near(b.Min, a.Min));
    CHECK(near(b.Max, a.Max));
  }

  TEST("the selection should stay sorted and unique")
  {
    board::Canvas canvas;
    canvas.select(5);
    canvas.selectAlso(2);
    canvas.selectAlso(9);
    canvas.selectAlso(5);
    CHECK((canvas.selection() == std::vector<board::BoxId>{2, 5, 9}));
    CHECK(canvas.isSelected(9));
    CHECK(!canvas.isSelected(3));
    canvas.deselect(5);
    canvas.deselect(4);
    CHECK((canvas.selection() == std::vector<board::BoxId>{2, 9}));
    canvas.deselectIf([](board::BoxId id) { return id > 5; });
    CHECK((canvas.selection() == std::vector<board::BoxId>{2}));
    canvas.select(7);
    CHECK((canvas.selection() == std::vector<board::BoxId>{7}));
    canvas.clearSelection();
    CHECK(canvas.selection().empty());
  }

  TEST("a click should select a box, the topmost under it")
  {
    Board b;
    b.frame();
    b.click(Board::screen(30.0f, 30.0f));
    CHECK((b.selection() == std::vector<board::BoxId>{1}));
    CHECK(!b.canvas.busy());
    b.click(Board::screen(80.0f, 60.0f)); // under box 3, over box 1
    CHECK((b.selection() == std::vector<board::BoxId>{3}));
    CHECK(b.canvas.hoveredBox() == 3);

    // Ctrl+click adds a box, and again takes it out.
    b.key(ImGuiMod_Ctrl, true);
    b.click(Board::screen(250.0f, 50.0f));
    CHECK((b.selection() == std::vector<board::BoxId>{2, 3}));
    b.click(Board::screen(80.0f, 60.0f));
    CHECK((b.selection() == std::vector<board::BoxId>{2}));
    b.key(ImGuiMod_Ctrl, false);

    // A click on empty canvas clears it.
    b.click(Board::screen(350.0f, 250.0f));
    CHECK(b.selection().empty());
  }

  TEST("dragging a selected box should move the whole selection")
  {
    Board b;
    b.frame();
    b.canvas.select(2);
    b.canvas.selectAlso(1);
    b.drag(Board::screen(30.0f, 30.0f),
           Board::screen(60.0f, 50.0f),
           ImGuiMouseButton_Left);
    CHECK(!b.canvas.busy());
    CHECK((b.selection() == std::vector<board::BoxId>{1, 2}));

    // The last moves reported: each box from where it started, by the drag.
    CHECK(b.moved.size() >= 2);
    ImVec2 one(0.0f, 0.0f), two(0.0f, 0.0f);

    for (const board::BoxMove& move : b.moved)
      (move.id == 1 ? one : two) = move.min;

    CHECK(near(one, ImVec2(50.0f, 40.0f)));
    CHECK(near(two, ImVec2(230.0f, 40.0f)));
  }

  TEST("dragging on empty canvas should select the boxes it overlaps")
  {
    Board b;
    b.frame();
    b.canvas.select(1);
    b.drag(Board::screen(330.0f, 200.0f),
           Board::screen(190.0f, 10.0f),
           ImGuiMouseButton_Left);
    CHECK((b.selection() == std::vector<board::BoxId>{2}));
    CHECK(!b.canvas.busy());

    // With Shift, it adds to the selection.
    b.key(ImGuiMod_Shift, true);
    b.drag(Board::screen(10.0f, 150.0f),
           Board::screen(70.0f, 130.0f),
           ImGuiMouseButton_Left);
    b.key(ImGuiMod_Shift, false);
    CHECK((b.selection() == std::vector<board::BoxId>{2, 3}));
  }

  TEST("a right click should be reported with its box, a right drag pan")
  {
    Board b;
    b.frame();
    b.click(Board::screen(250.0f, 50.0f), ImGuiMouseButton_Right);
    CHECK(b.rightClicked);
    CHECK(b.rightClickedBox == 2);
    CHECK((b.selection() == std::vector<board::BoxId>{2}));

    b.rightClicked = false;
    b.drag(Board::screen(300.0f, 200.0f),
           Board::screen(340.0f, 230.0f),
           ImGuiMouseButton_Right);
    CHECK(!b.rightClicked);
    CHECK(!b.canvas.busy());
    CHECK(near(b.canvas.view().pan, ImVec2(40.0f, 30.0f)));
  }

  TEST("a middle drag should pan and the wheel zoom about the mouse")
  {
    Board b;
    b.frame();
    b.drag(Board::screen(300.0f, 200.0f),
           Board::screen(320.0f, 210.0f),
           ImGuiMouseButton_Middle);
    CHECK(near(b.canvas.view().pan, ImVec2(20.0f, 10.0f)));

    const ImVec2 mouse = Board::screen(150.0f, 100.0f);
    b.move(mouse);
    const ImVec2 under = b.canvas.view().toCanvas(mouse);
    ImGui::GetIO().AddMouseWheelEvent(0.0f, 1.0f);
    b.frame();
    CHECK(near(b.canvas.zoom(), 1.15f));
    CHECK(near(b.canvas.view().toScreen(under), mouse));

    // Held within the zoom limits.
    for (int i = 0; i < 40; ++i)
    {
      ImGui::GetIO().AddMouseWheelEvent(0.0f, -1.0f);
      b.frame();
    }

    CHECK(near(b.canvas.zoom(), b.canvas.settings().minZoom));
  }

  TEST("a left press the board user claims should be its own gesture")
  {
    Board b;
    int offered = 0;
    b.claimPress = [&](ImVec2)
    {
      ++offered;
      return board::Press::Gesture;
    };
    b.frame();
    b.move(Board::screen(30.0f, 30.0f));
    b.button(ImGuiMouseButton_Left, true);
    CHECK(offered == 1);
    CHECK(b.canvas.gesture() == board::Gesture::Custom);
    CHECK(!b.gestureEnded);
    b.move(Board::screen(90.0f, 30.0f));
    b.button(ImGuiMouseButton_Left, false);
    CHECK(b.gestureEnded);
    CHECK(!b.canvas.busy());
    // Not a click on the box under it, nor a move.
    CHECK(b.selection().empty());
    CHECK(b.moved.empty());

    // Handled at once: no gesture, and the box under it isn't selected.
    b.claimPress = [](ImVec2) { return board::Press::Handled; };
    b.click(Board::screen(30.0f, 30.0f));
    CHECK(b.selection().empty());
    CHECK(!b.canvas.busy());
  }

  TEST("a frame request should fit the selection, or everything, once settled")
  {
    Board b;
    const auto applyFrame = [&]
    {
      b.gui.frame(
          [&]
          {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
            ImGui::Begin("Board", nullptr, ImGuiWindowFlags_NoTitleBar);

            if (b.canvas.begin("board"))
              b.canvas.applyFrame(b.boxes);

            ImGui::End();
          });
    };

    // Not on the first frame: the canvas hasn't settled at its size.
    applyFrame();
    CHECK(b.canvas.zoom() == 1.0f);
    applyFrame();
    const board::View& v = b.canvas.view();
    const ImVec2 middle = v.origin + v.size * 0.5f;
    // Every box: 20..300 x 20..140, with its margin.
    CHECK(near(v.toScreen({160.0f, 80.0f}), middle));
    CHECK(near(v.zoom, std::min(v.size.x / 400.0f, v.size.y / 240.0f)));

    b.canvas.select(2);
    b.canvas.frame();
    applyFrame();
    CHECK(near(v.toScreen({250.0f, 50.0f}), middle));
    CHECK(near(v.zoom, b.canvas.settings().fitMaxZoom));

    b.canvas.frameRect(ImRect({1000.0f, 1000.0f}, {1100.0f, 1050.0f}));
    applyFrame();
    CHECK(near(v.toScreen({1050.0f, 1025.0f}), middle));
  }

  TEST("boxes and links should draw faded when dimmed, and not off view")
  {
    Gui gui;
    board::Canvas canvas;
    // The highest alpha among a drawing's vertices, and how many it made.
    const auto drawn = [&](const std::function<void(ImDrawList*)>& paint)
    {
      int count = 0, alpha = 0;
      gui.frame(
          [&]
          {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
            ImGui::Begin("Board", nullptr, ImGuiWindowFlags_NoTitleBar);

            if (canvas.begin("board"))
            {
              ImDrawList* draw = canvas.beginDraw();
              const int start = draw->VtxBuffer.Size;
              paint(draw);

              for (int i = start; i < draw->VtxBuffer.Size; ++i)
                alpha = std::max(alpha,
                                 static_cast<int>(draw->VtxBuffer[i].col >>
                                                  IM_COL32_A_SHIFT));

              count = draw->VtxBuffer.Size - start;
              canvas.endDraw(draw);
            }

            ImGui::End();
          });
      return std::pair(count, alpha);
    };

    const board::Box box{1, {20.0f, 20.0f}, {160.0f, 80.0f}};
    const auto [plain, plainAlpha] =
        drawn([&](ImDrawList* draw)
              { CHECK(canvas.drawBox(draw, box, {.title = "Box"})); });
    CHECK(plain > 0);
    CHECK(plainAlpha == 255);
    const auto [dimmed, dimmedAlpha] =
        drawn([&](ImDrawList* draw)
              { canvas.drawBox(draw, box, {.title = "Box", .dimmed = true}); });
    CHECK(dimmed == plain);
    CHECK(dimmedAlpha < 255 / 2);

    const board::Box away{2, {2000.0f, 20.0f}, {2100.0f, 80.0f}};
    const auto [culled, culledAlpha] = drawn(
        [&](ImDrawList* draw)
        {
          CHECK(!canvas.drawBox(draw, away, {.title = "Box"}));
          canvas.drawLink(draw, {2000.0f, 0.0f}, {2100.0f, 50.0f}, {});
        });
    CHECK(culled == 0);

    const auto [line, lineAlpha] = drawn(
        [&](ImDrawList* draw)
        {
          canvas.drawLink(draw,
                          {20.0f, 20.0f},
                          {200.0f, 120.0f},
                          {.shape = board::LinkShape::Straight});
        });
    const auto [arrow, arrowAlpha] = drawn(
        [&](ImDrawList* draw)
        {
          canvas.drawLink(draw,
                          {20.0f, 20.0f},
                          {200.0f, 120.0f},
                          {.arrow = true, .shape = board::LinkShape::Straight});
        });
    CHECK(line > 0);
    CHECK(arrow > line); // its head

    // A path: drawn with its corners rounded and an arrowhead, culled when
    // it's all outside the view, and nothing for a path with no length.
    const std::array<ImVec2, 4> path = {ImVec2(20.0f, 20.0f),
                                        {100.0f, 20.0f},
                                        {100.0f, 120.0f},
                                        {200.0f, 120.0f}};
    const auto [rounded, roundedAlpha] =
        drawn([&](ImDrawList* draw)
              { CHECK(canvas.drawPath(draw, path, {.cornerRadius = 10.0f})); });
    const auto [sharp, sharpAlpha] =
        drawn([&](ImDrawList* draw)
              { CHECK(canvas.drawPath(draw, path, {.cornerRadius = 0.0f})); });
    const auto [headed, headedAlpha] =
        drawn([&](ImDrawList* draw)
              { CHECK(canvas.drawPath(draw, path, {.arrow = true})); });
    CHECK(sharp > 0);
    CHECK(rounded > sharp); // the curves round the corners
    CHECK(headed > rounded);

    const std::array<ImVec2, 2> offscreen = {
        ImVec2(2000.0f, 0.0f), {2100.0f, 0.0f}};
    const std::array<ImVec2, 3> still = {
        ImVec2(50.0f, 50.0f), {50.0f, 50.0f}, {50.0f, 50.0f}};
    const auto [skipped, skippedAlpha] = drawn(
        [&](ImDrawList* draw)
        {
          CHECK(!canvas.drawPath(draw, offscreen, {.arrow = true}));
          CHECK(!canvas.drawPath(draw, still, {}));
        });
    CHECK(skipped == 0);
  }

  TEST("a point's distance to a path should be to its nearest segment")
  {
    const std::array<ImVec2, 4> path = {
        ImVec2(0.0f, 0.0f), {100.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 50.0f}};
    CHECK(near(board::pathDistance({50.0f, 10.0f}, path), 10.0f));
    CHECK(near(board::pathDistance({110.0f, 25.0f}, path), 10.0f));
    CHECK(near(board::pathDistance({100.0f, 0.0f}, path), 0.0f));
    // Past its end: to the end point.
    CHECK(near(board::pathDistance({100.0f, 80.0f}, path), 30.0f));
    CHECK(near(board::pathDistance({-30.0f, -40.0f}, path), 50.0f));
    CHECK(board::pathDistance({0.0f, 0.0f}, {}) == FLT_MAX);
    const std::array<ImVec2, 1> dot = {ImVec2(3.0f, 4.0f)};
    CHECK(near(board::pathDistance({0.0f, 0.0f}, dot), 5.0f));

    const ImRect bounds = board::pathBounds(path);
    CHECK(near(bounds.Min, ImVec2(0.0f, 0.0f)));
    CHECK(near(bounds.Max, ImVec2(100.0f, 50.0f)));
    CHECK(board::pathBounds({}).Min.x > board::pathBounds({}).Max.x);
  }

  TEST("a box should be sized to its title and lines")
  {
    Gui gui;
    gui.frame(
        [&]
        {
          const board::BoxStyle style;
          const ImVec2 empty = board::boxSize(style, "A", {});
          CHECK(near(empty, ImVec2(style.minWidth, style.headerHeight)));

          const std::string longTitle(80, 'W');
          CHECK(board::boxSize(style, longTitle, {}).x > style.minWidth);

          const std::array<std::string, 3> lines{"one", "two", "three"};
          const ImVec2 withLines = board::boxSize(style, "A", lines);
          CHECK(near(withLines.y,
                     style.headerHeight + 3 * style.lineHeight +
                         style.padding * 0.5f));
        });
  }

  return testing::report("boardCanvasTests");
}
