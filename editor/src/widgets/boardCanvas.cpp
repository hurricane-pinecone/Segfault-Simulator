#include "widgets/boardCanvas.h"

#include "widgets/theme.h"

#include <imgui.h>
#include <imgui_internal.h> // ImRect, ImLengthSqr, Bézier closest points

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace board
{

namespace
{

// A right-drag past this (screen pixels) pans; one short of it is a click.
constexpr float RIGHT_DRAG = 4.0f;

// The handles of a curved link reach at least this far (canvas units).
constexpr float CURVE_HANDLE = 40.0f;

// A link leaving and entering on one side bows out this much of the height
// between its ends.
constexpr float BOW = 0.25f;

// How faded a dimmed box or link is: its alpha is scaled by this.
constexpr float DIMMED_ALPHA = 0.25f;

// An arrowhead's length (canvas units), and the shortest it's drawn (pixels).
constexpr float ARROW_LENGTH = 10.0f;
constexpr float ARROW_MIN_LENGTH = 6.0f;

ImU32 faded(ImU32 color, bool dimmed);

bool overlaps(const View& view, ImVec2 min, ImVec2 max, float pad);

} // namespace

// --- View --------------------------------------------------------------------

bool View::fit(const ImRect& bounds, float margin, float minZoom, float maxZoom)
{
  if (bounds.Min.x > bounds.Max.x || bounds.Min.y > bounds.Max.y ||
      size.x <= 0.0f || size.y <= 0.0f)
    return false;

  const ImVec2 extent = bounds.GetSize() + ImVec2(2 * margin, 2 * margin);
  zoom = std::clamp(
      std::min(size.x / extent.x, size.y / extent.y), minZoom, maxZoom);
  pan = size * 0.5f - bounds.GetCenter() * zoom;
  return true;
}

bool View::shows(ImVec2 min, ImVec2 max, float pad) const
{
  return overlaps(*this, toScreen(min), toScreen(max), pad);
}

bool View::showsLink(ImVec2 from,
                     ImVec2 to,
                     LinkShape shape,
                     float pad,
                     LinkSide leaves,
                     LinkSide entersFrom) const
{
  // A Bézier curve stays inside its control points' bounds.
  if (shape == LinkShape::Straight)
    return overlaps(*this, ImMin(from, to), ImMax(from, to), pad);

  const std::array<ImVec2, 4> points =
      curve(from, to, zoom, leaves, entersFrom);
  ImVec2 min = points[0], max = points[0];

  for (const ImVec2& point : points)
  {
    min = ImMin(min, point);
    max = ImMax(max, point);
  }

  return overlaps(*this, min, max, pad);
}

ImVec2 View::toCanvas(ImVec2 screen) const
{
  return (screen - origin - pan) * (1.0f / zoom);
}

ImVec2 View::toScreen(ImVec2 canvas) const
{
  return origin + pan + canvas * zoom;
}

void View::zoomAbout(ImVec2 screen, float newZoom)
{
  const ImVec2 under = toCanvas(screen);
  zoom = newZoom;
  pan = screen - origin - under * zoom;
}

// --- Canvas ------------------------------------------------------------------

void Canvas::applyFrame(std::span<const Box> boxes)
{
  const bool requested = m_frameRequested || m_frameRect;

  if (!requested || !m_settled || m_view.size.x <= 64.0f ||
      m_view.size.y <= 64.0f)
    return;

  m_frameRequested = false;
  ImRect bounds(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));

  if (m_frameRect)
    bounds = *std::exchange(m_frameRect, std::nullopt);
  else
    for (const Box& box : boxes)
      if (m_selection.empty() || isSelected(box.id))
        bounds.Add(ImRect(box.min, box.max));

  m_view.fit(
      bounds, m_settings.fitMargin, m_settings.minZoom, m_settings.fitMaxZoom);
}

bool Canvas::begin(const char* id)
{
  m_view.origin = ImGui::GetCursorScreenPos();
  const ImVec2 size = ImGui::GetContentRegionAvail();
  m_settled = size.x == m_view.size.x && size.y == m_view.size.y;
  m_view.size = size;

  if (size.x < 2.0f || size.y < 2.0f)
    return false;

  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton(id,
                         size,
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonRight |
                             ImGuiButtonFlags_MouseButtonMiddle);
  return true;
}

ImDrawList* Canvas::beginDraw() const
{
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 max = m_view.origin + m_view.size;
  draw->PushClipRect(m_view.origin, max, true);
  draw->AddRectFilled(m_view.origin, max, m_settings.background);
  drawGrid(draw);
  return draw;
}

void Canvas::deselect(BoxId id)
{
  const auto it = std::lower_bound(m_selection.begin(), m_selection.end(), id);

  if (it != m_selection.end() && *it == id)
    m_selection.erase(it);
}

bool Canvas::drawBox(ImDrawList* draw,
                     const Box& box,
                     const BoxLook& look) const
{
  const float zoom = m_view.zoom;
  const ImVec2 min = m_view.toScreen(box.min);
  const ImVec2 max = m_view.toScreen(box.max);

  if (!overlaps(m_view, min, max, m_style.overhang * zoom + 2.0f))
    return false;

  const float rounding = m_style.rounding * zoom;
  const float fontSize = this->fontSize();
  const bool showText = fontSize >= m_style.smallestText;
  ImFont* font = ImGui::GetFont();
  const auto text = [&](ImVec2 at, ImU32 color, std::string_view value)
  {
    if (showText && !value.empty())
      draw->AddText(font,
                    fontSize,
                    at,
                    faded(color, look.dimmed),
                    value.data(),
                    value.data() + value.size());
  };

  draw->AddRectFilled(min, max, faded(m_style.fill, look.dimmed), rounding);
  const ImVec2 headerMax(max.x, min.y + m_style.headerHeight * zoom);
  draw->AddRectFilled(min,
                      headerMax,
                      faded(look.header, look.dimmed),
                      rounding,
                      ImDrawFlags_RoundCornersTop);
  text(min + ImVec2(m_style.padding * zoom,
                    (m_style.headerHeight * zoom - fontSize) * 0.5f),
       m_style.titleColor,
       look.title);

  const float textOffset = (m_style.lineHeight * zoom - fontSize) * 0.5f;
  float y = headerMax.y;

  for (const std::string& line : look.lines)
  {
    text(ImVec2(min.x + m_style.padding * zoom, y + textOffset),
         m_style.lineColor,
         line);
    y += m_style.lineHeight * zoom;
  }

  // An emphasis first, then the selection, then the hover.
  const ImU32 hoverOutline =
      look.hovered ? m_style.hoveredOutline : m_style.outline;
  const ImU32 selectOutline = look.selected ? theme::kAccent : hoverOutline;
  const ImU32 outline = look.emphasis ? look.emphasis : selectOutline;
  const bool thick = look.emphasis || look.selected;
  draw->AddRect(min,
                max,
                faded(outline, look.dimmed),
                rounding,
                0,
                thick ? std::max(2.0f * zoom, 1.5f) : 1.0f);
  return true;
}

void Canvas::drawGrid(ImDrawList* draw) const
{
  const float step = m_settings.gridStep * m_view.zoom;

  if (step < m_settings.minGridSpacing)
    return;

  const ImVec2 origin = m_view.origin;
  const ImVec2 size = m_view.size;
  const ImVec2 offset(
      std::fmod(m_view.pan.x, step), std::fmod(m_view.pan.y, step));

  for (float x = offset.x; x < size.x; x += step)
    draw->AddLine(origin + ImVec2(x, 0),
                  origin + ImVec2(x, size.y),
                  m_settings.gridColor);

  for (float y = offset.y; y < size.y; y += step)
    draw->AddLine(origin + ImVec2(0, y),
                  origin + ImVec2(size.x, y),
                  m_settings.gridColor);
}

void Canvas::drawLink(ImDrawList* draw,
                      ImVec2 from,
                      ImVec2 to,
                      const LinkLook& look) const
{
  const ImVec2 a = m_view.toScreen(from);
  const ImVec2 b = m_view.toScreen(to);
  const float width = std::max(look.width * m_view.zoom, look.minWidth);
  const float arrow = look.arrow ? ARROW_LENGTH * m_view.zoom : 0.0f;

  if (!m_view.showsLink(
          a, b, look.shape, width + arrow + 2.0f, look.leaves, look.entersFrom))
    return;

  drawScreenLink(draw, a, b, look);
}

bool Canvas::drawPath(ImDrawList* draw,
                      std::span<const ImVec2> points,
                      const PathLook& look) const
{
  if (points.size() < 2)
    return false;

  const float zoom = m_view.zoom;
  const float width = std::max(look.width * zoom, look.minWidth);
  const float arrow =
      look.arrow ? std::max(ARROW_LENGTH * zoom, ARROW_MIN_LENGTH) : 0.0f;
  const ImRect bounds = pathBounds(points);

  if (!m_view.shows(bounds.Min, bounds.Max, width + arrow + 2.0f))
    return false;

  // The last segment that has a length: the arrowhead lies along it, and the
  // line stops at its base.
  const ImVec2 tip = m_view.toScreen(points.back());
  std::size_t before = points.size() - 1;
  float length = 0.0f;

  while (before-- > 0)
  {
    length = std::sqrt(ImLengthSqr(tip - m_view.toScreen(points[before])));

    if (length > 0.01f)
      break;
  }

  if (length <= 0.01f)
    return false;

  const ImU32 color = faded(look.color, look.dimmed);
  const ImVec2 direction =
      (tip - m_view.toScreen(points[before])) * (1.0f / length);
  ImVec2 end = tip;

  if (look.arrow)
  {
    const float head = std::min(arrow, length);
    const ImVec2 side(-direction.y * arrow * 0.5f, direction.x * arrow * 0.5f);
    end = tip - direction * head;
    draw->AddTriangleFilled(tip, end + side, end - side, color);
  }

  // Each corner rounded off, by at most half of either segment beside it.
  const float radius = look.cornerRadius * zoom;
  ImVec2 previous = m_view.toScreen(points.front());
  draw->PathLineTo(previous);

  for (std::size_t k = 1; k <= before; ++k)
  {
    const ImVec2 corner = m_view.toScreen(points[k]);
    const ImVec2 next = k == before ? end : m_view.toScreen(points[k + 1]);
    const float in = std::sqrt(ImLengthSqr(corner - previous));
    const float out = std::sqrt(ImLengthSqr(next - corner));

    if (in < 0.01f || out < 0.01f)
      continue;

    const float r = std::min({radius, in * 0.5f, out * 0.5f});
    draw->PathLineTo(corner + (previous - corner) * (r / in));
    draw->PathBezierQuadraticCurveTo(
        corner, corner + (next - corner) * (r / out));
    previous = corner;
  }

  draw->PathLineTo(end);
  draw->PathStroke(color, ImDrawFlags_None, width);
  return true;
}

void Canvas::drawScreenLink(ImDrawList* draw,
                            ImVec2 from,
                            ImVec2 to,
                            const LinkLook& look) const
{
  const float zoom = m_view.zoom;
  const ImU32 color = faded(look.color, look.dimmed);
  const float width = std::max(look.width * zoom, look.minWidth);
  ImVec2 end = to;

  // The arrowhead's tip is the link's end: the line stops at its base.
  if (look.arrow)
  {
    const float heading = look.entersFrom == LinkSide::Left ? 1.0f : -1.0f;
    ImVec2 direction(heading, 0.0f);

    if (look.shape == LinkShape::Straight)
    {
      const float length = std::sqrt(ImLengthSqr(to - from));

      if (length > 0.0f)
        direction = (to - from) * (1.0f / length);
    }

    const float length = std::max(ARROW_LENGTH * zoom, ARROW_MIN_LENGTH);
    const ImVec2 side(
        -direction.y * length * 0.5f, direction.x * length * 0.5f);
    end = to - direction * length;
    draw->AddTriangleFilled(to, end + side, end - side, color);
  }

  if (look.shape == LinkShape::Straight)
  {
    draw->AddLine(from, end, color, width);
    return;
  }

  const std::array<ImVec2, 4> points =
      curve(from, end, zoom, look.leaves, look.entersFrom);
  draw->AddBezierCubic(
      points[0], points[1], points[2], points[3], color, width);
}

void Canvas::endDraw(ImDrawList* draw) const
{
  if (m_gesture == Gesture::BoxSelect)
  {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    draw->AddRectFilled(m_pressedAt, mouse, theme::kAccentSoft);
    draw->AddRect(m_pressedAt, mouse, theme::kAccent);
  }

  draw->PopClipRect();
}

float Canvas::fontSize() const { return ImGui::GetFontSize() * m_view.zoom; }

void Canvas::frame()
{
  m_frameRequested = true;
  m_frameRect.reset();
}

void Canvas::frameRect(const ImRect& rect)
{
  m_frameRect = rect;
  m_frameRequested = false;
}

Input Canvas::handleMouse(std::span<const Box> boxes,
                          const std::function<Press(ImVec2 mouse)>& claimPress)
{
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 mouse = io.MousePos;
  const bool hovered = ImGui::IsItemHovered();
  Input input;
  m_moved.clear();

  // Zoom about the mouse.
  if (hovered && io.MouseWheel != 0.0f && m_gesture != Gesture::MoveBoxes)
    m_view.zoomAbout(mouse,
                     std::clamp(m_view.zoom * std::pow(m_settings.wheelZoomStep,
                                                       io.MouseWheel),
                                m_settings.minZoom,
                                m_settings.maxZoom));

  const Box* under = hovered ? boxAt(boxes, m_view.toCanvas(mouse)) : nullptr;
  m_hovered = under ? under->id : 0;

  // Left button: the board user's own gestures, then moving, then selecting.
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      m_gesture == Gesture::None)
  {
    m_pressedAt = mouse;
    const bool adding = io.KeyCtrl || io.KeyShift;
    const Press press = claimPress ? claimPress(mouse) : Press::Unhandled;

    if (press == Press::Gesture)
      m_gesture = Gesture::Custom;
    else if (press == Press::Unhandled && under)
    {
      const BoxId id = under->id;

      if (adding)
      {
        if (isSelected(id))
          deselect(id);
        else
          selectAlso(id);
      }
      else if (!isSelected(id))
        select(id);

      if (isSelected(id))
      {
        m_moveStart.clear();

        for (const Box& box : boxes)
          if (isSelected(box.id))
            m_moveStart.push_back({box.id, box.min});

        m_gesture = Gesture::MoveBoxes;
      }
    }
    else if (press == Press::Unhandled)
    {
      if (!adding)
        m_selection.clear();

      m_gesture = Gesture::BoxSelect;
    }
  }

  if (hovered && under && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    input.doubleClickedBox = under->id;

  // Panning: the middle button, or dragging the right one.
  if (hovered && m_gesture == Gesture::None &&
      (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
       ImGui::IsMouseDragging(ImGuiMouseButton_Right, RIGHT_DRAG)))
    m_gesture = Gesture::Pan;

  switch (m_gesture)
  {
  case Gesture::Pan:
    m_view.pan = m_view.pan + io.MouseDelta;

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right))
      m_gesture = Gesture::None;

    return input; // a right-drag isn't a right-click
  case Gesture::MoveBoxes:
  {
    const ImVec2 delta = (mouse - m_pressedAt) * (1.0f / m_view.zoom);

    for (const BoxMove& start : m_moveStart)
      m_moved.push_back(
          {start.id, ImVec2(start.min.x + delta.x, start.min.y + delta.y)});

    input.moved = m_moved;

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
      m_gesture = Gesture::None;
      input.moveEnded = true;
    }

    break;
  }
  case Gesture::BoxSelect:
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
      const ImRect rect =
          rectBetween(m_view.toCanvas(m_pressedAt), m_view.toCanvas(mouse));

      for (const Box& box : boxes)
        if (rect.Overlaps(ImRect(box.min, box.max)))
          selectAlso(box.id);

      m_gesture = Gesture::None;
    }

    break;
  case Gesture::Custom:
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
      m_gesture = Gesture::None;
      input.gestureEnded = true;
    }

    break;
  case Gesture::None:
    break;
  }

  // A right-click, not a drag: it selects the box under it, if it isn't.
  if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
      m_gesture == Gesture::None &&
      !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right, RIGHT_DRAG))
  {
    input.rightClicked = true;

    if (under)
    {
      if (!isSelected(under->id))
        select(under->id);

      input.rightClickedBox = under->id;
    }
  }

  return input;
}

bool Canvas::isSelected(BoxId id) const
{
  return std::binary_search(m_selection.begin(), m_selection.end(), id);
}

void Canvas::selectAlso(BoxId id)
{
  const auto it = std::lower_bound(m_selection.begin(), m_selection.end(), id);

  if (it == m_selection.end() || *it != id)
    m_selection.insert(it, id);
}

bool Canvas::textShown() const { return fontSize() >= m_style.smallestText; }

// --- free functions ----------------------------------------------------------

const Box* boxAt(std::span<const Box> boxes, ImVec2 point)
{
  for (auto it = boxes.rbegin(); it != boxes.rend(); ++it)
    if (ImRect(it->min, it->max).Contains(point))
      return &*it;

  return nullptr;
}

ImVec2 boxSize(const BoxStyle& style,
               std::string_view title,
               std::span<const std::string> lines)
{
  const auto textWidth = [](std::string_view text)
  { return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x; };
  float width = std::max(style.minWidth, textWidth(title) + 2 * style.padding);

  for (const std::string& line : lines)
    width = std::max(width, textWidth(line) + 2 * style.padding);

  if (lines.empty())
    return ImVec2(width, style.headerHeight);

  const float body = static_cast<float>(lines.size()) * style.lineHeight +
                     style.padding * 0.5f;
  return ImVec2(width, style.headerHeight + body);
}

std::array<ImVec2, 4>
curve(ImVec2 from, ImVec2 to, float zoom, LinkSide leaves, LinkSide entersFrom)
{
  float dx = std::max(std::abs(to.x - from.x) * 0.5f, CURVE_HANDLE * zoom);
  const float out = leaves == LinkSide::Right ? dx : -dx;
  const float in = entersFrom == LinkSide::Left ? -dx : dx;

  // Out and back in on one side: a bow, which must clear the boxes between.
  if ((out > 0.0f) == (in > 0.0f))
  {
    dx = std::max(dx, std::abs(to.y - from.y) * BOW);
    return {from,
            from + ImVec2(out > 0.0f ? dx : -dx, 0),
            to + ImVec2(in > 0.0f ? dx : -dx, 0),
            to};
  }

  return {from, from + ImVec2(out, 0), to + ImVec2(in, 0), to};
}

float linkDistance(ImVec2 point,
                   ImVec2 from,
                   ImVec2 to,
                   LinkShape shape,
                   float zoom,
                   LinkSide leaves,
                   LinkSide entersFrom)
{
  if (shape == LinkShape::Straight)
    return std::sqrt(ImLengthSqr(ImLineClosestPoint(from, to, point) - point));

  // ImGui's own tolerance for flattening curves to draw them.
  const float tolerance = 1.25f;
  const std::array<ImVec2, 4> points =
      curve(from, to, zoom, leaves, entersFrom);
  const ImVec2 closest = ImBezierCubicClosestPointCasteljau(
      points[0], points[1], points[2], points[3], point, tolerance);
  return std::sqrt(ImLengthSqr(closest - point));
}

ImRect pathBounds(std::span<const ImVec2> points)
{
  ImRect bounds(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));

  for (const ImVec2& point : points)
    bounds.Add(point);

  return bounds;
}

float pathDistance(ImVec2 point, std::span<const ImVec2> points)
{
  if (points.empty())
    return FLT_MAX;

  float nearest = ImLengthSqr(points.front() - point);

  // A segment with no length is its point, already counted.
  for (std::size_t k = 1; k < points.size(); ++k)
    nearest = std::min(
        nearest,
        ImLengthSqr(points[k - 1] - points[k]) > 0.0f
            ? ImLengthSqr(ImLineClosestPoint(points[k - 1], points[k], point) -
                          point)
            : ImLengthSqr(points[k] - point));

  return std::sqrt(nearest);
}

ImRect rectBetween(ImVec2 a, ImVec2 b)
{
  return ImRect(ImMin(a, b), ImMax(a, b));
}

namespace
{

ImU32 faded(ImU32 color, bool dimmed)
{
  if (!dimmed)
    return color;

  const ImU32 alpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
  const auto scaled =
      static_cast<ImU32>(static_cast<float>(alpha) * DIMMED_ALPHA);
  return (color & ~IM_COL32_A_MASK) | (scaled << IM_COL32_A_SHIFT);
}

bool overlaps(const View& view, ImVec2 min, ImVec2 max, float pad)
{
  const ImVec2 viewMax = view.origin + view.size;
  return max.x >= view.origin.x - pad && max.y >= view.origin.y - pad &&
         min.x <= viewMax.x + pad && min.y <= viewMax.y + pad;
}

} // namespace

} // namespace board
