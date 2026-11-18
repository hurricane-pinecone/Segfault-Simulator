#include "architecture/routing.h"

#include <imgui.h>
#include <imgui_internal.h> // ImRect

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace architecture
{

namespace
{

// A box's sides, as its exits are kept.
enum Side : int
{
  LEFT,
  RIGHT,
};

// How far outside everything the lines round the edge run, and the room
// round them for lanes (canvas units).
constexpr float OUTER_GAP = 60.0f;
constexpr float OUTER_ROOM = 48.0f;

// Uses of one piece of channel are told apart to a tenth of a canvas unit.
constexpr float PIECE_GRID = 10.0f;

// Spread legs on one line keep this much apart end to end (canvas units).
constexpr float LANE_END_GAP = 2.0f;

void sortLines(std::vector<Router::Line>& lines, float merge);

int32_t snapped(float value)
{
  return static_cast<int32_t>(std::lround(value * PIECE_GRID));
}

bool collinear(const ImVec2& a, const ImVec2& b, const ImVec2& c)
{
  return (a.x == b.x && b.x == c.x) || (a.y == b.y && b.y == c.y);
}

} // namespace

void Router::assignLanes()
{
  std::vector<uint32_t>& order = m_order;
  order.clear();

  for (uint32_t leg = 0; leg < m_legs.size(); ++leg)
    if (m_legs[leg].channel >= 0)
      order.push_back(leg);

  std::sort(order.begin(),
            order.end(),
            [&](uint32_t a, uint32_t b)
            {
              const Leg& la = m_legs[a];
              const Leg& lb = m_legs[b];
              return std::tie(la.channel, la.group, la.link) <
                     std::tie(lb.channel, lb.group, lb.link);
            });

  // Each group's legs on a line, ordered by its average turn, then theirs.
  for (std::size_t at = 0; at < order.size();)
  {
    std::size_t end = at;
    float sum = 0.0f;

    while (end < order.size() &&
           m_legs[order[end]].channel == m_legs[order[at]].channel &&
           m_legs[order[end]].group == m_legs[order[at]].group)
      sum += m_legs[order[end++]].bias;

    for (std::size_t i = at; i < end; ++i)
      m_legs[order[i]].groupBias = sum / static_cast<float>(end - at);

    at = end;
  }

  std::sort(
      order.begin(),
      order.end(),
      [&](uint32_t a, uint32_t b)
      {
        const Leg& la = m_legs[a];
        const Leg& lb = m_legs[b];
        return std::tie(la.channel,
                        la.groupBias,
                        la.group,
                        la.bias,
                        la.lo,
                        la.link) <
               std::tie(
                   lb.channel, lb.groupBias, lb.group, lb.bias, lb.lo, lb.link);
      });

  m_lanes.assign(m_legs.size(), 0);
  std::size_t at = 0;

  while (at < order.size())
  {
    const int channel = m_legs[order[at]].channel;
    std::size_t end = at;

    while (end < order.size() && m_legs[order[end]].channel == channel)
      ++end;

    // A group's legs (one box's links) take a block of lanes side by side,
    // the lowest block free all along the group's stretch of the line.
    m_taken.clear();
    int lanes = 0;

    for (std::size_t group = at; group < end;)
    {
      const uint32_t key = m_legs[order[group]].group;
      std::size_t next = group;
      float lo = FLT_MAX, hi = -FLT_MAX;
      m_laneEnds.clear();

      for (; next < end && m_legs[order[next]].group == key; ++next)
      {
        const Leg& leg = m_legs[order[next]];
        lo = std::min(lo, leg.lo);
        hi = std::max(hi, leg.hi);
        std::size_t lane = 0;

        while (lane < m_laneEnds.size() &&
               m_laneEnds[lane] + LANE_END_GAP > leg.lo)
          ++lane;

        if (lane == m_laneEnds.size())
          m_laneEnds.push_back(leg.hi);
        else
          m_laneEnds[lane] = leg.hi;

        m_lanes[order[next]] = static_cast<int>(lane);
      }

      const auto width = static_cast<int>(m_laneEnds.size());
      int base = 0;

      for (bool clash = true; clash;)
      {
        clash = false;

        for (const auto& [lane, span] : m_taken)
          if (lane >= base && lane < base + width &&
              span.first < hi + LANE_END_GAP && lo < span.second + LANE_END_GAP)
          {
            clash = true;
            ++base;
            break;
          }
      }

      for (std::size_t i = group; i < next; ++i)
      {
        const Leg& leg = m_legs[order[i]];
        m_lanes[order[i]] += base;
        m_taken.push_back({m_lanes[order[i]], {leg.lo, leg.hi}});
        lanes = std::max(lanes, m_lanes[order[i]] + 1);
      }

      group = next;
    }

    // Lanes centred on the line, closer together where it's crowded.
    const float room = channel < static_cast<int>(m_xs.size())
                           ? m_xs[channel].room
                           : m_ys[channel - m_xs.size()].room;
    const float gap =
        lanes > 1 ? std::min(m_settings.laneGap,
                             2.0f * room / static_cast<float>(lanes - 1))
                  : 0.0f;
    const float middle = static_cast<float>(lanes - 1) * 0.5f;

    for (std::size_t i = at; i < end; ++i)
      m_legs[order[i]].offset =
          (static_cast<float>(m_lanes[order[i]]) - middle) * gap;

    at = end;
  }
}

void Router::assignPorts(std::span<const ImRect> boxes,
                         std::span<const RouteLink> links,
                         Routes& out)
{
  // Each link's two ends on its boxes' sides, sorted per side by where the
  // link heads next: those going up leave from the top.
  m_uses.clear();

  for (uint32_t link = 0; link < links.size(); ++link)
  {
    if (!out.routed[link])
      continue;

    const uint32_t first = out.cornerStart[link];
    const uint32_t last = out.cornerStart[link + 1];
    const ImVec2* c = out.corners.data() + first;
    const uint32_t n = last - first;
    const uint32_t legs = m_legStart[link];

    Use leaving;
    leaving.key[0] = static_cast<int32_t>(links[link].from);
    leaving.key[1] = c[1].x > c[0].x ? RIGHT : LEFT;
    leaving.order = c[2].y;
    leaving.order2 = c[0].y;
    leaving.link = link;
    leaving.index = legs;
    m_uses.push_back(leaving);

    Use entering;
    entering.key[0] = static_cast<int32_t>(links[link].to);
    entering.key[1] = c[n - 2].x > c[n - 1].x ? RIGHT : LEFT;
    entering.order = c[n - 3].y;
    entering.order2 = c[n - 1].y;
    entering.link = link;
    entering.index = legs + n - 2;
    m_uses.push_back(entering);
  }

  std::sort(m_uses.begin(),
            m_uses.end(),
            [](const Use& a, const Use& b)
            {
              return std::tie(a.key[0], a.key[1], a.order, a.index) <
                     std::tie(b.key[0], b.key[1], b.order, b.index);
            });

  for (std::size_t at = 0; at < m_uses.size();)
  {
    std::size_t end = at;

    while (end < m_uses.size() && m_uses[end].key[0] == m_uses[at].key[0] &&
           m_uses[end].key[1] == m_uses[at].key[1])
      ++end;

    const auto slots = static_cast<float>(end - at);
    const float height = boxes[m_uses[at].key[0]].GetHeight();
    const float gap =
        slots > 1.0f
            ? std::min(m_settings.laneGap,
                       (height - 2.0f * m_settings.portMargin) / (slots - 1.0f))
            : 0.0f;

    // Each end kept on its side, whatever its height bundled (a link to its
    // own box leaves and enters off the middle).
    const ImRect& box = boxes[m_uses[at].key[0]];
    const float top = box.Min.y + m_settings.portMargin;
    const float bottom = std::max(top, box.Max.y - m_settings.portMargin);

    for (std::size_t i = at; i < end; ++i)
    {
      const float offset =
          (static_cast<float>(i - at) - (slots - 1.0f) * 0.5f) *
          std::max(gap, 0.0f);
      const float y = m_uses[i].order2;
      m_legs[m_uses[i].index].offset = std::clamp(y + offset, top, bottom) - y;
    }

    at = end;
  }
}

void Router::buildChannels(std::span<const ImRect> boxes)
{
  lines(boxes, true);
  lines(boxes, false);

  // A box a line runs through is off the grid (dragged there, say): it gets
  // lines of its own round it, so links can go round it.
  const float c = m_settings.clearance;
  const std::size_t xLines = m_xs.size(), yLines = m_ys.size();

  for (const ImRect& box : boxes)
  {
    const auto through = [&](const std::vector<Line>& lines,
                             std::size_t count,
                             float lo,
                             float hi)
    {
      return std::any_of(lines.begin(),
                         lines.begin() + static_cast<std::ptrdiff_t>(count),
                         [&](const Line& line)
                         { return line.at > lo && line.at < hi; });
    };

    if (!through(m_xs, xLines, box.Min.x - c, box.Max.x + c) &&
        !through(m_ys, yLines, box.Min.y - c, box.Max.y + c))
      continue;

    for (const float x : {box.Min.x - c, box.Max.x + c})
      m_xs.push_back({x, c * 0.5f});

    for (const float y : {box.Min.y - c, box.Max.y + c})
      m_ys.push_back({y, c * 0.5f});
  }

  sortLines(m_xs, c * 0.5f);
  sortLines(m_ys, c * 0.5f);

  // A line's lanes stay nearer it than the next line's: no two lines' lanes
  // meet.
  for (std::vector<Line>* lines : {&m_xs, &m_ys})
    for (std::size_t i = 0; i < lines->size(); ++i)
    {
      Line& line = (*lines)[i];

      if (i > 0)
        line.room =
            std::min(line.room, (line.at - (*lines)[i - 1].at) * 0.5f - 0.5f);

      if (i + 1 < lines->size())
        line.room =
            std::min(line.room, ((*lines)[i + 1].at - line.at) * 0.5f - 0.5f);
    }

  // Where boxes block each line: a box blocks a line that comes within
  // nearly its clearance of it.
  const float near = c * 0.9f;
  m_blockedX.resize(m_xs.size());
  m_blockedY.resize(m_ys.size());

  for (auto& blocked : m_blockedX)
    blocked.clear();

  for (auto& blocked : m_blockedY)
    blocked.clear();

  for (const ImRect& box : boxes)
  {
    for (std::size_t i = 0; i < m_xs.size(); ++i)
      if (m_xs[i].at > box.Min.x - near && m_xs[i].at < box.Max.x + near)
        m_blockedX[i].push_back({box.Min.y - near, box.Max.y + near});

    for (std::size_t j = 0; j < m_ys.size(); ++j)
      if (m_ys[j].at > box.Min.y - near && m_ys[j].at < box.Max.y + near)
        m_blockedY[j].push_back({box.Min.x - near, box.Max.x + near});
  }

  const std::size_t v = m_xs.size(), h = m_ys.size();
  m_clear.assign(v * (h - 1) + h * (v - 1), 0);

  for (std::size_t i = 0; i < v; ++i)
    for (std::size_t j = 0; j + 1 < h; ++j)
      m_clear[i * (h - 1) + j] =
          clearAlong(static_cast<int>(i), m_ys[j].at, m_ys[j + 1].at) ? 1 : 0;

  for (std::size_t j = 0; j < h; ++j)
    for (std::size_t i = 0; i + 1 < v; ++i)
    {
      const float lo = m_xs[i].at, hi = m_xs[i + 1].at;
      const bool clear =
          std::none_of(m_blockedY[j].begin(),
                       m_blockedY[j].end(),
                       [&](const std::pair<float, float>& span)
                       { return span.first < hi && span.second > lo; });
      m_clear[v * (h - 1) + j * (v - 1) + i] = clear ? 1 : 0;
    }
}

void Router::buildPieces(Routes& out)
{
  // Every stretch between two points of a route, as found, under a key of
  // its two ends either way round: the links sharing one are a piece.
  m_uses.clear();

  for (uint32_t link = 0; link < m_pointRange.size(); ++link)
  {
    const auto [first, last] = m_pointRange[link];

    for (uint32_t k = first; k + 1 < last; ++k)
    {
      const ImVec2 a = m_points[k].at;
      const ImVec2 b = m_points[k + 1].at;

      if (a.x == b.x && a.y == b.y)
        continue;

      Use use;
      const bool flip = std::tie(b.x, b.y) < std::tie(a.x, a.y);
      const ImVec2 lo = flip ? b : a;
      const ImVec2 hi = flip ? a : b;
      use.key[0] = snapped(lo.x);
      use.key[1] = snapped(lo.y);
      use.key[2] = snapped(hi.x);
      use.key[3] = snapped(hi.y);
      use.link = link;
      use.index = k;
      use.enters = out.routed[link] && k + 2 == last;
      m_uses.push_back(use);
    }
  }

  std::sort(m_uses.begin(),
            m_uses.end(),
            [](const Use& a, const Use& b)
            {
              return std::tie(a.key[0], a.key[1], a.key[2], a.key[3], a.link) <
                     std::tie(b.key[0], b.key[1], b.key[2], b.key[3], b.link);
            });

  out.pieces.clear();
  out.pieceLinks.clear();

  for (std::size_t at = 0; at < m_uses.size();)
  {
    std::size_t end = at;
    const Use& first = m_uses[at];

    while (end < m_uses.size() && std::equal(std::begin(first.key),
                                             std::end(first.key),
                                             std::begin(m_uses[end].key)))
      ++end;

    RoutePiece piece;
    piece.a = m_points[first.index].at;
    piece.b = m_points[first.index + 1].at;
    piece.firstLink = static_cast<uint32_t>(out.pieceLinks.size());

    for (std::size_t i = at; i < end; ++i)
    {
      const Use& use = m_uses[i];

      if (out.pieceLinks.size() == piece.firstLink ||
          out.pieceLinks.back() != use.link)
        out.pieceLinks.push_back(use.link);

      // Entering a box: its end at the box goes last, for the arrowhead.
      if (use.enters && !piece.entersBox)
      {
        piece.entersBox = true;
        piece.a = m_points[use.index].at;
        piece.b = m_points[use.index + 1].at;
      }
    }

    piece.linkCount =
        static_cast<uint32_t>(out.pieceLinks.size()) - piece.firstLink;
    out.pieces.push_back(piece);
    at = end;
  }
}

bool Router::clearAlong(int line, float y0, float y1) const
{
  const float lo = std::min(y0, y1), hi = std::max(y0, y1);
  return std::none_of(m_blockedX[line].begin(),
                      m_blockedX[line].end(),
                      [&](const std::pair<float, float>& span)
                      { return span.first < hi && span.second > lo; });
}

void Router::findExits(std::span<const ImRect> boxes)
{
  const float c = m_settings.clearance;
  const float near = c * 0.9f;
  m_exits.assign(boxes.size() * 2, Exit());

  for (std::size_t box = 0; box < boxes.size(); ++box)
  {
    const ImRect& rect = boxes[box];
    const float y = rect.GetCenter().y;

    for (const Side side : {LEFT, RIGHT})
    {
      // The nearest x line out from the side, past its clearance, that the
      // link can go up or down from; further out only while the stub across
      // to it stays clear (a box dragged close to others reaches past them
      // through a gap between rows).
      const float sideX = side == RIGHT ? rect.Max.x : rect.Min.x;
      const int count = static_cast<int>(m_xs.size());
      const int step = side == RIGHT ? 1 : -1;

      for (int line = side == RIGHT ? 0 : count - 1; line >= 0 && line < count;
           line += step)
      {
        const float x = m_xs[line].at;

        if (side == RIGHT ? x < rect.Max.x + c * 0.5f
                          : x > rect.Min.x - c * 0.5f)
          continue;

        const float lo = std::min(sideX, x);
        const float hi = std::max(sideX, x);
        bool clear = true;

        for (std::size_t other = 0; other < boxes.size() && clear; ++other)
        {
          const ImRect& o = boxes[other];
          clear =
              other == box || !(o.Min.x - near < hi && o.Max.x + near > lo &&
                                o.Min.y - near < y && o.Max.y + near > y);
        }

        if (!clear)
          break;

        Exit exit;
        exit.line = line;
        exit.stub = hi - lo;

        for (std::size_t j = 0; j < m_ys.size(); ++j)
        {
          if (m_ys[j].at < y)
            exit.up = static_cast<int>(j);
          else if (exit.down < 0)
            exit.down = static_cast<int>(j);
        }

        if (exit.up >= 0 && !clearAlong(line, m_ys[exit.up].at, y))
          exit.up = -1;

        if (exit.down >= 0 && !clearAlong(line, y, m_ys[exit.down].at))
          exit.down = -1;

        if (exit.up < 0 && exit.down < 0)
          continue;

        m_exits[box * 2 + side] = exit;
        break;
      }
    }
  }
}

void Router::lines(std::span<const ImRect> boxes, bool vertical)
{
  // How many boxes (with their clearance) cover each stretch across the
  // board: a line goes through each stretch covered less than those either
  // side of it, the middle of a gutter or a row gap.
  const float c = m_settings.clearance;
  m_events.clear();
  float lowest = FLT_MAX, highest = -FLT_MAX;

  for (const ImRect& box : boxes)
  {
    const float lo = (vertical ? box.Min.x : box.Min.y) - c;
    const float hi = (vertical ? box.Max.x : box.Max.y) + c;
    m_events.push_back({lo, 1});
    m_events.push_back({hi, -1});
    lowest = std::min(lowest, lo);
    highest = std::max(highest, hi);
  }

  std::sort(m_events.begin(), m_events.end());
  std::vector<Line>& out = vertical ? m_xs : m_ys;
  out.clear();

  if (boxes.empty())
    return;

  out.push_back({lowest - OUTER_GAP, OUTER_ROOM});

  // Runs of one cover, merged.
  std::vector<Run>& runs = m_runs;
  runs.clear();
  int cover = 0;

  for (std::size_t e = 0; e < m_events.size(); ++e)
  {
    cover += m_events[e].second;

    if (e + 1 == m_events.size())
      break;

    const float lo = m_events[e].first, hi = m_events[e + 1].first;

    if (hi <= lo)
      continue;

    if (!runs.empty() && runs.back().cover == cover)
      runs.back().hi = hi;
    else
      runs.push_back({lo, hi, cover});
  }

  for (std::size_t r = 1; r + 1 < runs.size(); ++r)
  {
    const Run& run = runs[r];

    if (run.cover < runs[r - 1].cover && run.cover < runs[r + 1].cover)
      out.push_back(
          {(run.lo + run.hi) * 0.5f, (run.hi - run.lo) * 0.5f + c * 0.5f});
  }

  out.push_back({highest + OUTER_GAP, OUTER_ROOM});
}

void Router::markShared(uint32_t from, uint32_t to)
{
  for (const int segment : m_segments)
  {
    ++m_load[segment];
    uint64_t* bits = m_shared.data() + segment * m_words;
    bits[from >> 6] |= uint64_t{1} << (from & 63);
    bits[to >> 6] |= uint64_t{1} << (to & 63);
  }
}

void Router::route(std::span<const ImRect> boxes,
                   std::span<const RouteLink> links,
                   Routes& out)
{
  out.cornerStart.assign(1, 0);
  out.corners.clear();
  out.spread.clear();
  out.bounds.clear();
  out.routed.clear();
  out.pieces.clear();
  out.pieceLinks.clear();

  if (boxes.empty())
    return;

  buildChannels(boxes);
  findExits(boxes);

  const std::size_t count = boxes.size();
  m_words = (count + 63) / 64;
  const std::size_t segments = m_clear.size() + count * 4;
  m_shared.assign(segments * m_words, 0);
  m_load.assign(segments, 0);

  // The lanes each segment has room for; a box's exit isn't counted.
  const std::size_t v = m_xs.size(), h = m_ys.size();
  const std::size_t verticals = v * (h - 1);
  m_capacity.assign(segments, FLT_MAX);

  for (std::size_t i = 0; i < verticals; ++i)
    m_capacity[i] = 1.0f + 2.0f * m_xs[i / (h - 1)].room / m_settings.laneGap;

  for (std::size_t i = verticals; i < m_clear.size(); ++i)
    m_capacity[i] =
        1.0f + 2.0f * m_ys[(i - verticals) / (v - 1)].room / m_settings.laneGap;

  // Each box's links in a row, the box with the most first, each one's
  // furthest first: the long trunks are laid and the rest branch off them.
  std::vector<uint32_t>& order = m_order;
  order.resize(links.size());
  std::vector<float>& leaving = m_laneEnds;
  leaving.assign(count, 0.0f);

  for (uint32_t i = 0; i < links.size(); ++i)
  {
    order[i] = i;
    leaving[links[i].from] += 1.0f;
  }

  const auto distance = [&](const RouteLink& link)
  {
    const ImVec2 a = boxes[link.from].GetCenter();
    const ImVec2 b = boxes[link.to].GetCenter();
    return std::abs(a.x - b.x) + std::abs(a.y - b.y);
  };

  std::sort(
      order.begin(),
      order.end(),
      [&](uint32_t a, uint32_t b)
      {
        const RouteLink& la = links[a];
        const RouteLink& lb = links[b];
        return std::make_tuple(-leaving[la.from], la.from, -distance(la), a) <
               std::make_tuple(-leaving[lb.from], lb.from, -distance(lb), b);
      });

  m_points.clear();
  m_pointRange.assign(links.size(), {0, 0});

  for (const uint32_t link : order)
  {
    const auto first = static_cast<uint32_t>(m_points.size());
    routeOne(boxes, links[link]);
    m_pointRange[link] = {first, static_cast<uint32_t>(m_points.size())};
  }

  // Each route's corners: its turns, and always where it leaves and meets
  // the gutters by its boxes, so a link's port and lane can differ.
  out.routed.assign(links.size(), 0);
  m_legs.clear();
  m_legStart.assign(1, 0);
  std::vector<Point>& kept = m_kept;

  for (uint32_t link = 0; link < links.size(); ++link)
  {
    const auto [first, last] = m_pointRange[link];
    const std::span<const Point> points(m_points.data() + first, last - first);
    const std::size_t n = points.size();
    out.routed[link] = n >= 4 ? 1 : 0;
    kept.clear();

    for (std::size_t k = 0; k < n; ++k)
    {
      const bool end = k < 2 || k + 2 >= n;

      if (!end && collinear(kept.back().at, points[k].at, points[k + 1].at))
        continue;

      kept.push_back(points[k]);
    }

    for (const Point& point : kept)
      out.corners.push_back(point.at);

    out.cornerStart.push_back(static_cast<uint32_t>(out.corners.size()));

    // Its legs: along an x line, a y line, or a stub at a box's side. The
    // first half go with the box it leaves, the rest with the one it enters.
    const std::size_t legs = kept.size() - 1;

    for (std::size_t k = 0; k < legs; ++k)
    {
      const Point& a = kept[k];
      const Point& b = kept[k + 1];
      Leg leg;
      leg.link = link;

      if (a.xLine >= 0 && a.xLine == b.xLine)
      {
        leg.channel = a.xLine;
        leg.lo = std::min(a.at.y, b.at.y);
        leg.hi = std::max(a.at.y, b.at.y);
      }
      else
      {
        if (a.yLine >= 0 && a.yLine == b.yLine)
          leg.channel = static_cast<int>(m_xs.size()) + a.yLine;

        leg.lo = std::min(a.at.x, b.at.x);
        leg.hi = std::max(a.at.x, b.at.x);
      }

      leg.group = k * 2 < legs ? links[link].from
                               : links[link].to + static_cast<uint32_t>(count);

      // The legs before and after turn off it one way or the other.
      const bool vertical =
          leg.channel >= 0 && leg.channel < static_cast<int>(m_xs.size());
      const auto across = [&](const ImVec2& from, const ImVec2& to)
      {
        const float d = vertical ? to.x - from.x : to.y - from.y;
        return static_cast<float>((d > 0.0f) - (d < 0.0f));
      };

      if (k > 0)
        leg.bias += across(a.at, kept[k - 1].at);

      if (k + 2 < kept.size())
        leg.bias += across(b.at, kept[k + 2].at);

      m_legs.push_back(leg);
    }

    m_legStart.push_back(static_cast<uint32_t>(m_legs.size()));
  }

  assignLanes();
  assignPorts(boxes, links, out);

  // Each corner moves with the lanes of the legs either side of it: an x
  // line's lane moves it across, a y line's or a port's up or down.
  out.spread.assign(out.corners.size(), ImVec2(0.0f, 0.0f));
  out.bounds.resize(links.size());

  for (uint32_t link = 0; link < links.size(); ++link)
  {
    const uint32_t first = out.cornerStart[link];
    const uint32_t last = out.cornerStart[link + 1];
    const uint32_t legs = m_legStart[link];
    ImRect bounds(out.corners[first], out.corners[first]);

    for (uint32_t c = first; c < last; ++c)
    {
      const uint32_t k = c - first;
      ImVec2& spread = out.spread[c];

      for (const uint32_t leg : {k - 1, k})
      {
        if (leg == UINT32_MAX || leg >= last - first - 1)
          continue;

        const Leg& along = m_legs[legs + leg];
        const bool vertical =
            along.channel >= 0 && along.channel < static_cast<int>(m_xs.size());

        if (vertical)
          spread.x = along.offset;
        else
          spread.y = along.offset;
      }

      bounds.Add(out.corners[c]);
      bounds.Add(
          ImVec2(out.corners[c].x + spread.x, out.corners[c].y + spread.y));
    }

    out.bounds[link] = bounds;
  }

  buildPieces(out);
}

void Router::routeOne(std::span<const ImRect> boxes, const RouteLink& link)
{
  const ImRect& from = boxes[link.from];
  const ImRect& to = boxes[link.to];
  const float bend = m_settings.bendCost;
  const int v = static_cast<int>(m_xs.size());
  const int h = static_cast<int>(m_ys.size());
  const int verticals = v * (h - 1);
  const int grid = static_cast<int>(m_clear.size());
  const auto sideX = [](const ImRect& box, int side)
  { return side == RIGHT ? box.Max.x : box.Min.x; };
  const auto exitSegment = [&](uint32_t box, int side, int down)
  { return grid + static_cast<int>(box) * 4 + side * 2 + down; };
  const auto point = [&](float x, float y, int xLine, int yLine) {
    m_points.push_back({ImVec2(x, y), xLine, yLine});
  };
  float fromY = from.GetCenter().y;
  float toY = to.GetCenter().y;
  m_segments.clear();

  // A box's link to itself: out of a side and back in lower down.
  if (link.from == link.to)
  {
    for (const int side : {RIGHT, LEFT})
    {
      const Exit& exit = m_exits[link.from * 2 + side];

      if (exit.line < 0)
        continue;

      const float quarter = from.GetHeight() * 0.25f;
      const float x = m_xs[exit.line].at;
      point(sideX(from, side), fromY - quarter, -1, -1);
      point(x, fromY - quarter, exit.line, -1);
      point(x, fromY + quarter, exit.line, -1);
      point(sideX(from, side), fromY + quarter, -1, -1);
      return;
    }
  }

  // Boxes beside one gutter, or one above the other with a gutter alongside
  // both: straight along it.
  float best = FLT_MAX;
  int bestFrom = -1, bestTo = -1;

  for (const int out : {RIGHT, LEFT})
    for (const int in : {LEFT, RIGHT})
    {
      const Exit& leave = m_exits[link.from * 2 + out];
      const Exit& enter = m_exits[link.to * 2 + in];

      if (leave.line < 0 || leave.line != enter.line ||
          !clearAlong(leave.line, fromY, toY))
        continue;

      const float rise = std::abs(toY - fromY);
      const float cost =
          leave.stub + enter.stub + rise + (rise > 0.5f ? 2.0f * bend : 0.0f);

      if (cost < best)
      {
        best = cost;
        bestFrom = out;
        bestTo = in;
      }
    }

  if (bestFrom >= 0)
  {
    const int line = m_exits[link.from * 2 + bestFrom].line;
    const float x = m_xs[line].at;
    point(sideX(from, bestFrom), fromY, -1, -1);
    point(x, fromY, line, -1);
    point(x, toY, line, -1);
    point(sideX(to, bestTo), toY, -1, -1);
    return;
  }

  // Otherwise the cheapest way over the channel grid (A*). A state is a
  // crossing and whether it was reached across (0) or along (1) its x line,
  // so bends are paid for; the last state is the target box. The estimate
  // left to go is the distance to the target's side and height at the
  // cheapest a channel can cost, so it never overestimates.
  const float cheapest = std::min(m_settings.shareFactor, 1.0f);
  const auto estimate = [&](int state)
  {
    if (state >= v * h * 2)
      return 0.0f;

    const int node = state / 2;
    const float x = m_xs[node % v].at;
    const float y = m_ys[node / v].at;
    const float across = std::max({to.Min.x - x, 0.0f, x - to.Max.x});
    return cheapest * (across + std::abs(y - toY));
  };
  const int states = v * h * 2;
  const int target = states;
  m_cost.resize(states + 1);
  m_parent.resize(states + 1);
  m_seen.resize(states + 1, 0);

  if (++m_stamp == 0)
  {
    std::fill(m_seen.begin(), m_seen.end(), 0);
    m_stamp = 1;
  }

  m_heap.clear();
  const auto reach = [&](int state, float cost, int parent)
  {
    if (m_seen[state] == m_stamp && m_cost[state] <= cost)
      return false;

    m_seen[state] = m_stamp;
    m_cost[state] = cost;
    m_parent[state] = parent;
    m_heap.push_back({-(cost + estimate(state)), state});
    std::push_heap(m_heap.begin(), m_heap.end());
    return true;
  };

  // Out of either side, up or down its gutter to a row line; the parent of
  // a first state says which (-1 - side * 2 - down).
  for (const int side : {RIGHT, LEFT})
  {
    const Exit& exit = m_exits[link.from * 2 + side];

    for (const int down : {0, 1})
    {
      const int row = down ? exit.down : exit.up;

      if (exit.line < 0 || row < 0)
        continue;

      const float length = exit.stub + std::abs(m_ys[row].at - fromY);
      const float cost =
          segmentCost(
              exitSegment(link.from, side, down), length, link.from, link.to) +
          bend;
      reach((row * v + exit.line) * 2 + 1, cost, -1 - side * 2 - down);
    }
  }

  // Into either side, from a row line up or down its gutter.
  struct Entry
  {
    int node = -1;
    float cost = 0.0f;
    int side = 0;
    int down = 0;
  };
  Entry entries[4];
  int entryCount = 0;

  for (const int side : {LEFT, RIGHT})
  {
    const Exit& exit = m_exits[link.to * 2 + side];

    for (const int down : {0, 1})
    {
      const int row = down ? exit.down : exit.up;

      if (exit.line < 0 || row < 0)
        continue;

      const float length = exit.stub + std::abs(m_ys[row].at - toY);
      entries[entryCount++] = {
          row * v + exit.line,
          segmentCost(
              exitSegment(link.to, side, down), length, link.from, link.to) +
              bend,
          side,
          down};
    }
  }

  int entered = -1;

  while (!m_heap.empty())
  {
    std::pop_heap(m_heap.begin(), m_heap.end());
    const auto [negative, state] = m_heap.back();
    m_heap.pop_back();

    if (state == target)
      break;

    // Reached more cheaply since this was queued.
    const float cost = m_cost[state];

    if (-negative > cost + estimate(state) + 0.01f)
      continue;

    const int node = state / 2;
    const bool along = state % 2 == 1;
    const int i = node % v, j = node / v;
    const float turnAlong = along ? 0.0f : bend;
    const float turnAcross = along ? bend : 0.0f;

    if (j > 0 && m_clear[i * (h - 1) + j - 1])
      reach(((j - 1) * v + i) * 2 + 1,
            cost + turnAlong +
                segmentCost(i * (h - 1) + j - 1,
                            m_ys[j].at - m_ys[j - 1].at,
                            link.from,
                            link.to),
            state);

    if (j + 1 < h && m_clear[i * (h - 1) + j])
      reach(((j + 1) * v + i) * 2 + 1,
            cost + turnAlong +
                segmentCost(i * (h - 1) + j,
                            m_ys[j + 1].at - m_ys[j].at,
                            link.from,
                            link.to),
            state);

    if (i > 0 && m_clear[verticals + j * (v - 1) + i - 1])
      reach((j * v + i - 1) * 2,
            cost + turnAcross +
                segmentCost(verticals + j * (v - 1) + i - 1,
                            m_xs[i].at - m_xs[i - 1].at,
                            link.from,
                            link.to),
            state);

    if (i + 1 < v && m_clear[verticals + j * (v - 1) + i])
      reach((j * v + i + 1) * 2,
            cost + turnAcross +
                segmentCost(verticals + j * (v - 1) + i,
                            m_xs[i + 1].at - m_xs[i].at,
                            link.from,
                            link.to),
            state);

    for (int e = 0; e < entryCount; ++e)
      if (entries[e].node == node &&
          reach(target, cost + turnAlong + entries[e].cost, state))
        entered = e;
  }

  // No clear way: straight between the sides facing each other.
  if (entered < 0 || m_seen[target] != m_stamp)
  {
    const bool rightwards = to.GetCenter().x >= from.GetCenter().x;
    point(sideX(from, rightwards ? RIGHT : LEFT), fromY, -1, -1);
    point(sideX(to, rightwards ? LEFT : RIGHT), toY, -1, -1);
    return;
  }

  // Back from the target to the first state, then out in order.
  const std::size_t start = m_points.size();
  const Entry& entry = entries[entered];
  const Exit& in = m_exits[link.to * 2 + entry.side];
  point(sideX(to, entry.side), toY, -1, -1);
  point(m_xs[in.line].at, toY, in.line, -1);
  m_segments.push_back(exitSegment(link.to, entry.side, entry.down));
  int state = m_parent[target];
  int previous = -1;

  while (state >= 0)
  {
    const int node = state / 2;
    const int i = node % v, j = node / v;
    point(m_xs[i].at, m_ys[j].at, i, j);

    if (previous >= 0)
    {
      const int pi = previous % v, pj = previous / v;
      m_segments.push_back(pi == i ? i * (h - 1) + std::min(j, pj)
                                   : verticals + j * (v - 1) + std::min(i, pi));
    }

    previous = node;
    state = m_parent[state];
  }

  const int code = -1 - state;
  const int side = code / 2;
  const Exit& out = m_exits[link.from * 2 + side];
  m_segments.push_back(exitSegment(link.from, side, code % 2));
  point(m_xs[out.line].at, fromY, out.line, -1);
  point(sideX(from, side), fromY, -1, -1);
  std::reverse(
      m_points.begin() + static_cast<std::ptrdiff_t>(start), m_points.end());
  markShared(link.from, link.to);
}

float Router::segmentCost(int segment,
                          float length,
                          uint32_t from,
                          uint32_t to) const
{
  const uint64_t* bits = m_shared.data() + segment * m_words;
  const bool shared = (bits[from >> 6] >> (from & 63) & 1) != 0 ||
                      (bits[to >> 6] >> (to & 63) & 1) != 0;
  const float over =
      static_cast<float>(m_load[segment]) + 1.0f - m_capacity[segment];
  const float crowd = 1.0f + std::max(over, 0.0f) * m_settings.crowdCost;
  return (shared ? length * m_settings.shareFactor : length) * crowd;
}

void routePath(const Routes& routes,
               uint32_t link,
               float spread,
               std::vector<ImVec2>& out)
{
  out.clear();

  for (uint32_t c = routes.cornerStart[link]; c < routes.cornerStart[link + 1];
       ++c)
    out.push_back(ImVec2(routes.corners[c].x + routes.spread[c].x * spread,
                         routes.corners[c].y + routes.spread[c].y * spread));
}

namespace
{

// Sort lines, merging those nearer than `merge`: the one with more room
// stays.
void sortLines(std::vector<Router::Line>& lines, float merge)
{
  std::sort(lines.begin(),
            lines.end(),
            [](const Router::Line& a, const Router::Line& b)
            { return std::tie(a.at, a.room) < std::tie(b.at, b.room); });
  std::size_t kept = 0;

  for (const Router::Line& line : lines)
  {
    if (kept > 0 && line.at - lines[kept - 1].at < merge)
    {
      if (line.room > lines[kept - 1].room)
        lines[kept - 1] = line;

      continue;
    }

    lines[kept++] = line;
  }

  lines.resize(kept);
}

} // namespace

} // namespace architecture
