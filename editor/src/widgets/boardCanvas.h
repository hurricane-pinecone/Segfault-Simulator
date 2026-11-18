#pragma once

#include "widgets/theme.h"

#include <imgui.h>
#include <imgui_internal.h> // ImRect

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A node board: an endless grey-gridded plane of boxes joined by links, to pan
// around, zoom about the mouse, select boxes on and drag them about. It draws
// and edits what it's handed each frame and owns none of it: the boxes'
// meaning, contents and positions stay with whoever uses it.
//
//   - Middle-drag or right-drag pans; the wheel zooms about the mouse.
//   - Click a box to select it, Ctrl+ or Shift+click to add or remove one, and
//     drag to move the selection; drag on empty space to select with a box.
//   - A right click (not a drag) is reported, with the box under it.
//
// A panel's frame, with its boxes laid out in canvas units:
//
//   if (!m_board.begin("board"))
//     return;
//
//   m_board.applyFrame(boxes);
//   const board::Input input = m_board.handleMouse(boxes);
//
//   for (const board::BoxMove& move : input.moved)
//     positionOf(move.id) = move.min;
//
//   ImDrawList* draw = m_board.beginDraw();
//   m_board.drawLink(draw, from, to, linkLook);
//   m_board.drawBox(draw, box, boxLook);
//   m_board.endDraw(draw);
namespace board
{

/** A box's id: whatever the board's user names its boxes by. 0 is no box. */
using BoxId = uint64_t;

/** A gesture the mouse is making on the board. */
enum class Gesture
{
  None,
  Pan,
  MoveBoxes,
  BoxSelect,
  // One of the board user's own, started from a left press it handled.
  Custom,
};

/** What the board user did with a left press it was offered. */
enum class Press
{
  // Not its: the board selects, moves or box-selects.
  Unhandled,
  // Done with at once (a click on something of its own).
  Handled,
  // Its own drag begins: Gesture::Custom, until the left button is released.
  Gesture,
};

enum class LinkShape
{
  // A curve leaving its start and entering its end level, on the sides its
  // look names: by default an S-curve leaving rightwards and entering from the
  // left, as node graphs draw their links.
  Curved,
  Straight,
};

/** A side of a box, where a curved link leaves or enters it. */
enum class LinkSide
{
  Right,
  Left,
};

/** A box as laid out this frame. */
struct Box
{
  /** Who it is (never 0). */
  BoxId id = 0;
  /** Its top left corner, in canvas units. */
  ImVec2 min{0.0f, 0.0f};
  /** Its bottom right corner, in canvas units. */
  ImVec2 max{0.0f, 0.0f};
};

/** A box that moved, while the selection is dragged. */
struct BoxMove
{
  /** The box. */
  BoxId id = 0;
  /** Where its top left corner goes, in canvas units. */
  ImVec2 min{0.0f, 0.0f};
};

/** What the mouse did on the board this frame. */
struct Input
{
  /**
   * While the selection is dragged, each selected box's new top left corner
   * (until the next handleMouse() call).
   */
  std::span<const BoxMove> moved;
  /** A double click on this box (it's selected by the first click); 0 if none.
   */
  BoxId doubleClickedBox = 0;
  /** The board user's own gesture ended: the left button was released. */
  bool gestureEnded = false;
  /**
   * The selection was let go after being dragged (or a box was clicked): the
   * last `moved` were where the boxes stay.
   */
  bool moveEnded = false;
  /** A right click, not a drag: on rightClickedBox, or empty canvas (0). */
  bool rightClicked = false;
  /** The box right-clicked, now selected; 0 for empty canvas. */
  BoxId rightClickedBox = 0;
};

/** How the board pans, zooms and looks behind its boxes. */
struct Settings
{
  /** Grid lines' colour. */
  ImU32 gridColor = IM_COL32(255, 255, 255, 10);
  /** Grid spacing, in canvas units. */
  float gridStep = 32.0f;
  /** The background's colour. */
  ImU32 background = IM_COL32(34, 36, 35, 255);
  /** Margin round what's fitted in view, in canvas units. */
  float fitMargin = 60.0f;
  /** The closest fitting zooms in: small content isn't blown up. */
  float fitMaxZoom = 1.25f;
  /** The furthest the view zooms out. */
  float minZoom = 0.25f;
  /** The closest the view zooms in. */
  float maxZoom = 2.5f;
  /** The grid's lines hide when closer than this on screen (pixels). */
  float minGridSpacing = 6.0f;
  /** Zoom factor per wheel notch. */
  float wheelZoomStep = 1.15f;
};

/** The boxes' sizes and colours, in canvas units (screen pixels at zoom 1). */
struct BoxStyle
{
  /** The fill behind a box's contents. */
  ImU32 fill = theme::kTile;
  /** The header's height. */
  float headerHeight = 24.0f;
  /** A hovered box's outline. */
  ImU32 hoveredOutline = IM_COL32(120, 128, 122, 255);
  /** A body line's height. */
  float lineHeight = 21.0f;
  /** Body lines' text colour. */
  ImU32 lineColor = IM_COL32(214, 216, 210, 255);
  /** The narrowest a box is laid out. */
  float minWidth = 140.0f;
  /** The plain outline's colour. */
  ImU32 outline = IM_COL32(20, 22, 21, 255);
  /**
   * How far a box's drawing may reach past its rectangle (pins on its edges,
   * its outline), for culling, in canvas units.
   */
  float overhang = 8.0f;
  /** Space between a box's edge and its text. */
  float padding = 9.0f;
  /** Corner rounding. */
  float rounding = 5.0f;
  /** Text smaller than this on screen (pixels) isn't drawn. */
  float smallestText = 5.0f;
  /** The title's colour. */
  ImU32 titleColor = IM_COL32(236, 238, 232, 255);
};

/** One box's contents and state, for drawBox(). */
struct BoxLook
{
  /** The header's text. */
  std::string_view title;
  /** Text lines under the header, one per line height. */
  std::span<const std::string> lines;
  /** The header's colour. */
  ImU32 header = IM_COL32(80, 80, 80, 255);
  /**
   * An outline in this colour, thick, over the selected and hovered ones (an
   * error, say); 0 for none.
   */
  ImU32 emphasis = 0;
  /** Faded back, for highlighting other boxes. */
  bool dimmed = false;
  /** Under the mouse. */
  bool hovered = false;
  /** In the selection: outlined in the accent colour. */
  bool selected = false;
};

/** One link's look, for drawLink(). */
struct LinkLook
{
  /** An arrowhead at its end. */
  bool arrow = false;
  /** Its colour. */
  ImU32 color = IM_COL32_WHITE;
  /** Faded back, for highlighting other links. */
  bool dimmed = false;
  /**
   * A curved link's side of its end: from the left it arrives heading right,
   * from the right heading left.
   */
  LinkSide entersFrom = LinkSide::Left;
  /** A curved link's side of its start: it heads out that way. */
  LinkSide leaves = LinkSide::Right;
  /** The thinnest it's drawn on screen (pixels), zoomed out. */
  float minWidth = 1.2f;
  /** Curved or straight. */
  LinkShape shape = LinkShape::Curved;
  /** Its thickness, in canvas units. */
  float width = 2.0f;
};

/** One path's look, for drawPath(). */
struct PathLook
{
  /** An arrowhead at its last point, along its last segment. */
  bool arrow = false;
  /** Its colour. */
  ImU32 color = IM_COL32_WHITE;
  /** How round its corners are: their radius, in canvas units. */
  float cornerRadius = 10.0f;
  /** Faded back, for highlighting other paths. */
  bool dimmed = false;
  /** The thinnest it's drawn on screen (pixels), zoomed out. */
  float minWidth = 1.2f;
  /** Its thickness, in canvas units. */
  float width = 2.0f;
};

/**
 * Where the canvas is on screen and how it's panned and zoomed: canvas point
 * (0, 0) sits at origin + pan on screen, and a canvas unit is `zoom` pixels.
 */
struct View
{
  /**
   * Fit `bounds` in view: centred, as large as fits with `margin` round it,
   * the zoom kept within minZoom..maxZoom.
   *
   * @param bounds what to fit, in canvas units
   * @param margin space kept round it, in canvas units
   * @param minZoom the furthest out it zooms
   * @param maxZoom the closest in it zooms
   * @return false, changing nothing, if `bounds` is empty or the view has no
   *         size
   */
  bool fit(const ImRect& bounds, float margin, float minZoom, float maxZoom);

  /**
   * Whether a canvas-unit rectangle shows in the view, for culling what's
   * drawn.
   *
   * @param min its top left corner, in canvas units
   * @param max its bottom right corner, in canvas units
   * @param pad screen pixels it may reach past its rectangle (outlines, pins)
   * @return false if it lies wholly outside the view
   */
  bool shows(ImVec2 min, ImVec2 max, float pad) const;

  /**
   * Whether a link between two screen points shows in the view, for culling.
   *
   * @param from its start, on screen
   * @param to its end, on screen
   * @param shape how it's drawn
   * @param pad screen pixels it may reach past its curve (its thickness)
   * @param leaves a curved link's side of its start
   * @param entersFrom a curved link's side of its end
   * @return false if it lies wholly outside the view
   */
  bool showsLink(ImVec2 from,
                 ImVec2 to,
                 LinkShape shape,
                 float pad,
                 LinkSide leaves = LinkSide::Right,
                 LinkSide entersFrom = LinkSide::Left) const;

  /** A screen point's canvas position. */
  ImVec2 toCanvas(ImVec2 screen) const;

  /** A canvas point's screen position. */
  ImVec2 toScreen(ImVec2 canvas) const;

  /**
   * Zoom to `newZoom`, keeping the canvas point under `screen` where it is.
   *
   * @param screen the point that stays put, on screen
   * @param newZoom the zoom
   */
  void zoomAbout(ImVec2 screen, float newZoom);

  /** The canvas's top left corner, on screen. */
  ImVec2 origin{0.0f, 0.0f};
  /** Where canvas point (0, 0) is, from the origin (screen pixels). */
  ImVec2 pan{0.0f, 0.0f};
  /** The canvas's size on screen. */
  ImVec2 size{0.0f, 0.0f};
  /** Screen pixels per canvas unit. */
  float zoom = 1.0f;
};

/**
 * The board's mechanics: the view and its grid, the mouse's gestures, the
 * selection, and drawing boxes and links. See the top of this file.
 */
class Canvas
{
public:
  /**
   * Fit the view to a pending request (frame() or frameRect()) once the canvas
   * has settled at a usable size: a tab just docked takes a frame or two to.
   * Call after begin().
   *
   * @param boxes the boxes as laid out: frame() fits the selected ones, or
   *        every one if none is selected
   */
  void applyFrame(std::span<const Box> boxes);

  /**
   * Start the frame: the canvas takes the rest of the window, under an
   * invisible button that catches the mouse (other widgets drawn over it later
   * take the mouse where they are).
   *
   * @param id the button's ImGui id
   * @return false if there's no room to draw: skip the rest of the board
   */
  bool begin(const char* id);

  /**
   * Fill the canvas with the background and grid, and clip to it. Call after
   * handleMouse(), then draw the links and boxes, then endDraw().
   *
   * @return the list to draw into
   */
  ImDrawList* beginDraw() const;

  /** A gesture is under way: whatever is being edited is mid-edit. */
  bool busy() const { return m_gesture != Gesture::None; }

  /** Empty the selection. */
  void clearSelection() { m_selection.clear(); }

  /** Take `id` out of the selection, if it's in it. */
  void deselect(BoxId id);

  /** Take every box `remove` says yes to out of the selection. */
  template <typename Predicate>
  void deselectIf(Predicate&& remove)
  {
    std::erase_if(m_selection, remove);
  }

  /**
   * Draw a box: its fill, a header with its title, its body lines and an
   * outline for its state. Skipped while it's outside the view.
   *
   * @param draw the list from beginDraw()
   * @param box the box, in canvas units
   * @param look its contents and state
   * @return false if it was outside the view and not drawn
   */
  bool drawBox(ImDrawList* draw, const Box& box, const BoxLook& look) const;

  /**
   * Draw a link between two canvas points. Skipped while it's outside the
   * view.
   *
   * @param draw the list from beginDraw()
   * @param from its start, in canvas units
   * @param to its end, in canvas units
   * @param look how it's drawn
   */
  void drawLink(ImDrawList* draw,
                ImVec2 from,
                ImVec2 to,
                const LinkLook& look) const;

  /**
   * Draw a path through canvas points: straight segments with rounded
   * corners, and an arrowhead at its end if its look asks. Skipped while it's
   * outside the view.
   *
   * @param draw the list from beginDraw()
   * @param points its points, in canvas units (repeated points are skipped)
   * @param look how it's drawn
   * @return false if it was outside the view (or too short) and not drawn
   */
  bool drawPath(ImDrawList* draw,
                std::span<const ImVec2> points,
                const PathLook& look) const;

  /**
   * Draw a link between two screen points, unculled: one being dragged to
   * the mouse, say.
   *
   * @param draw the list from beginDraw()
   * @param from its start, on screen
   * @param to its end, on screen
   * @param look how it's drawn
   */
  void drawScreenLink(ImDrawList* draw,
                      ImVec2 from,
                      ImVec2 to,
                      const LinkLook& look) const;

  /**
   * Finish drawing: the box-select rectangle, if one's being dragged, then
   * the clip from beginDraw() comes off.
   *
   * @param draw the list from beginDraw()
   */
  void endDraw(ImDrawList* draw) const;

  /** Text's size on screen at the board's zoom (pixels). */
  float fontSize() const;

  /** Fit the selected boxes, or every box if none is, in view (applyFrame). */
  void frame();

  /** Fit a canvas-unit rectangle (a box's, say) in view (applyFrame). */
  void frameRect(const ImRect& rect);

  /** The gesture under way. */
  Gesture gesture() const { return m_gesture; }

  /**
   * Handle the mouse: zoom, pan, select, move the selection, box-select, and
   * the board user's own gestures. Call after begin() with no widget between
   * (it asks whether the canvas's button is hovered); a drop target on the
   * canvas may come between.
   *
   * @param boxes the boxes as laid out, in drawing order: the last is on top
   * @param claimPress offered a left press on the canvas (the mouse, on
   *        screen) before the boxes are, so the board user can start its own
   *        gesture from something on a box (a pin, say); may be empty
   * @return what the mouse did
   */
  Input handleMouse(std::span<const Box> boxes,
                    const std::function<Press(ImVec2 mouse)>& claimPress = {});

  /** The box under the mouse when handleMouse() last ran; 0 for none. */
  BoxId hoveredBox() const { return m_hovered; }

  /** Whether `id` is selected. */
  bool isSelected(BoxId id) const;

  /** Make the selection just `id`. */
  void select(BoxId id) { m_selection.assign(1, id); }

  /** Add `id` to the selection. */
  void selectAlso(BoxId id);

  /** The selected boxes, in ascending id order. */
  const std::vector<BoxId>& selection() const { return m_selection; }

  /** How the board pans and zooms; change it to retune the board. */
  Settings& settings() { return m_settings; }
  const Settings& settings() const { return m_settings; }

  /** The boxes' look; change it to restyle the board. */
  BoxStyle& style() { return m_style; }
  const BoxStyle& style() const { return m_style; }

  /** Whether text is big enough on screen to draw at the board's zoom. */
  bool textShown() const;

  /** Where the canvas is and how it's panned and zoomed. */
  const View& view() const { return m_view; }

  /** The zoom: screen pixels per canvas unit. */
  float zoom() const { return m_view.zoom; }

private:
  void drawGrid(ImDrawList* draw) const;

  std::optional<ImRect> m_frameRect;
  bool m_frameRequested = true;
  Gesture m_gesture = Gesture::None;
  BoxId m_hovered = 0;
  std::vector<BoxMove> m_moved;
  std::vector<BoxMove> m_moveStart; // MoveBoxes: where the boxes were
  ImVec2 m_pressedAt{0.0f, 0.0f};   // where the gesture began (screen)
  std::vector<BoxId> m_selection;   // sorted
  Settings m_settings;
  bool m_settled = false; // the canvas is the size it was last frame
  BoxStyle m_style;
  View m_view;
};

/**
 * The topmost box containing a point.
 *
 * @param boxes the boxes, in drawing order: the last is on top
 * @param point the point, in canvas units
 * @return the box, or null if there's none there
 */
const Box* boxAt(std::span<const Box> boxes, ImVec2 point);

/**
 * A box's size to fit its title and lines, laid out in `style` (canvas units).
 * Needs an ImGui frame, to measure text.
 *
 * @param style the board's box style
 * @param title the header's text
 * @param lines the body's lines
 * @return its width and height
 */
ImVec2 boxSize(const BoxStyle& style,
               std::string_view title,
               std::span<const std::string> lines);

/**
 * A curved link's Bézier control points: it leaves `from` and enters `to`
 * level, on the sides given (by default rightwards, and from the left), its
 * handles at least 40 canvas units long. A link leaving and entering on the
 * same side bows out further the further apart its ends are.
 *
 * @param from its start, on screen
 * @param to its end, on screen
 * @param zoom the view's zoom
 * @param leaves the side of its start it leaves from
 * @param entersFrom the side of its end it enters from
 * @return the four control points, from `from` to `to`
 */
std::array<ImVec2, 4> curve(ImVec2 from,
                            ImVec2 to,
                            float zoom,
                            LinkSide leaves = LinkSide::Right,
                            LinkSide entersFrom = LinkSide::Left);

/**
 * How far a screen point is from a link, for hit-testing links.
 *
 * @param point the point, on screen
 * @param from the link's start, on screen
 * @param to the link's end, on screen
 * @param shape how the link is drawn
 * @param zoom the view's zoom
 * @param leaves a curved link's side of its start
 * @param entersFrom a curved link's side of its end
 * @return the distance in screen pixels
 */
float linkDistance(ImVec2 point,
                   ImVec2 from,
                   ImVec2 to,
                   LinkShape shape,
                   float zoom,
                   LinkSide leaves = LinkSide::Right,
                   LinkSide entersFrom = LinkSide::Left);

/**
 * The rectangle round a path's points, for culling it.
 *
 * @param points its points
 * @return their bounds; empty (min past max) if there are none
 */
ImRect pathBounds(std::span<const ImVec2> points);

/**
 * How far a point is from a path of straight segments, for hit-testing it
 * (its rounded corners are near enough their corners).
 *
 * @param point the point
 * @param points the path's points, in the same units
 * @return the distance; FLT_MAX for a path with no points
 */
float pathDistance(ImVec2 point, std::span<const ImVec2> points);

/**
 * The rectangle dragged out between two corners, either way round.
 *
 * @param a one corner
 * @param b the opposite corner
 * @return the rectangle, its min at the top left
 */
ImRect rectBetween(ImVec2 a, ImVec2 b);

/** ImVec2 maths (ImGui keeps its own operators behind a define). */
inline ImVec2 operator+(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
inline ImVec2 operator-(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }
inline ImVec2 operator*(ImVec2 a, float s) { return {a.x * s, a.y * s}; }

} // namespace board
