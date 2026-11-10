#include "engine/runtime/navigation/laneGraph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace sfs::nav
{

namespace
{
// Lanes this much further than the nearest still taken as a trip's ends (the
// other way along the same road, a road beside it): the search picks.
constexpr float kEndSlack = 6.0f;
constexpr float kInf = std::numeric_limits<float>::infinity();
} // namespace

int LaneGraph::addNode(glm::vec2 at)
{
  m_nodes.push_back(at);
  m_out.emplace_back();
  return static_cast<int>(m_nodes.size()) - 1;
}

int LaneGraph::addLane(Lane lane)
{
  const int nodes = static_cast<int>(m_nodes.size());
  if (lane.line.size() < 2 || lane.from < 0 || lane.to < 0 ||
      lane.from >= nodes || lane.to >= nodes)
    return -1;
  std::vector<float> along{0.0f};
  for (std::size_t k = 1; k < lane.line.size(); ++k)
    along.push_back(along.back() +
                    glm::distance(lane.line[k - 1], lane.line[k]));
  lane.cost = std::max(lane.cost, 1.0e-3f);
  lane.speed = std::max(lane.speed, 0.1f);
  m_out[static_cast<std::size_t>(lane.from)].push_back(
      static_cast<int>(m_lanes.size()));
  m_lanes.push_back(std::move(lane));
  m_along.push_back(std::move(along));
  return static_cast<int>(m_lanes.size()) - 1;
}

std::vector<LaneGraph::Spot>
LaneGraph::near(glm::vec2 p, float reach, float slack) const
{
  std::vector<Spot> out;
  float best = kInf;
  for (std::size_t l = 0; l < m_lanes.size(); ++l)
  {
    const std::vector<glm::vec2>& line = m_lanes[l].line;
    Spot s{static_cast<int>(l), 0.0f, kInf};
    for (std::size_t i = 0; i + 1 < line.size(); ++i)
    {
      const glm::vec2 a = line[i], d = line[i + 1] - a;
      const float len2 = glm::dot(d, d);
      const float t = len2 > 0.0f
                          ? std::clamp(glm::dot(p - a, d) / len2, 0.0f, 1.0f)
                          : 0.0f;
      const float dist = glm::distance(p, a + d * t);
      if (dist < s.distance)
      {
        s.distance = dist;
        s.along = m_along[l][i] + t * std::sqrt(len2);
      }
    }
    if (s.distance <= reach)
    {
      out.push_back(s);
      best = std::min(best, s.distance);
    }
  }
  std::erase_if(out, [&](const Spot& s) { return s.distance > best + slack; });
  std::sort(out.begin(),
            out.end(),
            [](const Spot& a, const Spot& b)
            {
              return a.distance != b.distance ? a.distance < b.distance
                                              : a.lane < b.lane;
            });
  return out;
}

LaneGraph::Spot LaneGraph::nearest(glm::vec2 p, float within) const
{
  const std::vector<Spot> all = near(p, within, 0.0f);
  return all.empty() ? Spot{} : all.front();
}

glm::vec2 LaneGraph::pointAt(int lane, float along) const
{
  const std::vector<glm::vec2>& line =
      m_lanes[static_cast<std::size_t>(lane)].line;
  const std::vector<float>& at = m_along[static_cast<std::size_t>(lane)];
  along = std::clamp(along, 0.0f, at.back());
  const auto it = std::upper_bound(at.begin(), at.end(), along);
  const std::size_t i = std::min<std::size_t>(
      static_cast<std::size_t>(std::max<std::ptrdiff_t>(it - at.begin(), 1)) -
          1,
      line.size() - 2);
  const float span = std::max(at[i + 1] - at[i], 1.0e-6f);
  return glm::mix(line[i], line[i + 1], (along - at[i]) / span);
}

std::optional<std::vector<LaneGraph::Stretch>>
LaneGraph::route(glm::vec2 from, glm::vec2 to, const Options& o) const
{
  if (m_lanes.empty())
    return std::nullopt;
  const std::vector<Spot> ons = near(from, o.reach, kEndSlack);
  const std::vector<Spot> offs = near(to, o.reach, kEndSlack);
  if (ons.empty() || offs.empty() ||
      ons.front().distance + offs.front().distance >=
          o.worthIt * glm::distance(from, to))
    return std::nullopt;

  // A* over the nodes, plus the goal (index `goal`): getting on costs the
  // metres to the lane, each lane its metres times its cost, getting off the
  // metres from it. The heuristic: straight to the goal at the cheapest cost.
  struct Piece
  {
    int lane;
    float a0, a1;
  };
  float cheapest = 1.0f;
  for (const Lane& l : m_lanes)
    cheapest = std::min(cheapest, l.cost);
  const int goal = static_cast<int>(m_nodes.size());
  std::vector<float> g(m_nodes.size() + 1, kInf);
  // How each was reached: the node before (-1: from the start) and the piece.
  std::vector<int> prev(m_nodes.size() + 1, -2);
  std::vector<Piece> came(m_nodes.size() + 1, Piece{-1, 0.0f, 0.0f});
  using Item = std::pair<float, int>;
  std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
  const auto h = [&](int n)
  {
    if (n == goal)
      return 0.0f;
    // (Lanes run a little off their nodes: a few metres' slack.)
    return cheapest *
           std::max(glm::distance(m_nodes[static_cast<std::size_t>(n)], to) -
                        kEndSlack,
                    0.0f);
  };
  const auto reach = [&](int n, float cost, int before, Piece piece)
  {
    if (cost >= g[static_cast<std::size_t>(n)])
      return;
    g[static_cast<std::size_t>(n)] = cost;
    prev[static_cast<std::size_t>(n)] = before;
    came[static_cast<std::size_t>(n)] = piece;
    open.push({cost + h(n), n});
  };
  // Off a lane at a spot (the goal), `cost` so far at `a0` along it (from the
  // start along one lane: `before` -1).
  const auto offFrom = [&](int lane, float a0, float cost, int before)
  {
    for (const Spot& off : offs)
      if (off.lane == lane && off.along >= a0)
      {
        const float c =
            cost +
            (off.along - a0) * m_lanes[static_cast<std::size_t>(lane)].cost +
            off.distance;
        reach(goal, c, before, Piece{lane, a0, off.along});
      }
  };
  for (const Spot& on : ons)
  {
    const Lane& lane = m_lanes[static_cast<std::size_t>(on.lane)];
    const float length = m_along[static_cast<std::size_t>(on.lane)].back();
    offFrom(on.lane, on.along, on.distance, -1);
    reach(lane.to,
          on.distance + (length - on.along) * lane.cost,
          -1,
          Piece{on.lane, on.along, length});
  }
  std::vector<uint8_t> closed(m_nodes.size() + 1, 0);
  while (!open.empty())
  {
    const auto [f, n] = open.top();
    open.pop();
    if (closed[static_cast<std::size_t>(n)])
      continue;
    closed[static_cast<std::size_t>(n)] = 1;
    if (n == goal)
      break;
    for (const int l : m_out[static_cast<std::size_t>(n)])
    {
      const Lane& lane = m_lanes[static_cast<std::size_t>(l)];
      const float length = m_along[static_cast<std::size_t>(l)].back();
      const float c = g[static_cast<std::size_t>(n)];
      offFrom(l, 0.0f, c, n);
      reach(lane.to, c + length * lane.cost, n, Piece{l, 0.0f, length});
    }
  }
  if (!closed[static_cast<std::size_t>(goal)])
    return std::nullopt;
  std::vector<Piece> path;
  for (int n = goal; n != -1;)
  {
    path.push_back(came[static_cast<std::size_t>(n)]);
    n = prev[static_cast<std::size_t>(n)];
    if (n == -2)
      return std::nullopt; // (not reached: can't happen)
  }
  std::reverse(path.begin(), path.end());
  std::erase_if(path, [](const Piece& p) { return p.a1 - p.a0 < 1.0e-3f; });
  if (path.empty())
    return std::nullopt;

  // Each piece sampled every `step`; pieces meeting at a corner start a new
  // stretch, each cut back either side of it to leave room for the turn.
  const float step = std::max(o.step, 0.05f);
  struct Line
  {
    std::vector<glm::vec2> points;
    const Lane* lane;
  };
  std::vector<Line> lines;
  for (const Piece& p : path)
  {
    const float span = p.a1 - p.a0;
    const int steps = std::max(1, static_cast<int>(std::ceil(span / step)));
    Line line{{}, &m_lanes[static_cast<std::size_t>(p.lane)]};
    for (int k = 0; k <= steps; ++k)
      line.points.push_back(pointAt(
          p.lane,
          p.a0 + span * static_cast<float>(k) / static_cast<float>(steps)));
    lines.push_back(std::move(line));
  }
  const auto headingOf = [](const std::vector<glm::vec2>& line, bool atEnd)
  {
    const glm::vec2 d =
        atEnd ? line.back() - line[line.size() - 2] : line[1] - line.front();
    return std::atan2(d.y, d.x);
  };
  std::vector<Stretch> out;
  std::vector<glm::vec2> points;
  Stretch current;
  const auto finish = [&](float cutStart, float cutEnd)
  {
    float total = 0.0f;
    std::vector<float> along{0.0f};
    for (std::size_t k = 1; k < points.size(); ++k)
      along.push_back(total += glm::distance(points[k - 1], points[k]));
    std::vector<glm::vec2> kept;
    for (std::size_t k = 0; k < points.size(); ++k)
      if (along[k] >= cutStart && along[k] <= total - cutEnd)
        kept.push_back(points[k]);
    if (kept.size() >= 2 && total - cutStart - cutEnd >= o.least)
    {
      Stretch s = current;
      for (std::size_t k = 0; k < kept.size(); ++k)
      {
        const glm::vec2 d =
            kept[std::min(k + 1, kept.size() - 1)] - kept[k == 0 ? 0 : k - 1];
        s.poses.push_back({kept[k], std::atan2(d.y, d.x)});
      }
      for (std::size_t k = 0; k < kept.size(); ++k)
      {
        const std::size_t a = k == 0 ? 0 : k - 1,
                          b = std::min(k + 1, kept.size() - 1);
        const float travelled =
            std::max(glm::distance(kept[a], kept[b]), 1.0e-3f);
        s.curvature.push_back(
            wrapAngle(s.poses[b].heading - s.poses[a].heading) / travelled);
      }
      out.push_back(std::move(s));
    }
    points.clear();
  };
  float cutStart = o.lead;
  for (std::size_t i = 0; i < lines.size(); ++i)
  {
    const Line& line = lines[i];
    if (points.empty())
    {
      current = Stretch{};
      current.speed = line.lane->speed;
    }
    current.speed = std::min(current.speed, line.lane->speed);
    if (current.tags.empty() || current.tags.back() != line.lane->tag)
      current.tags.push_back(line.lane->tag);
    points.insert(points.end(),
                  line.points.begin() + (points.empty() ? 0 : 1),
                  line.points.end());
    if (i + 1 == lines.size())
      finish(cutStart, o.lead);
    else
    {
      const float turn =
          std::abs(wrapAngle(headingOf(lines[i + 1].points, false) -
                             headingOf(line.points, true)));
      if (turn > o.corner)
      {
        // (Room for a turn of `radius` through that angle, and a little more.)
        const float cut =
            o.radius * std::tan(0.5f * std::min(turn, 2.6f)) + 3.0f;
        finish(cutStart, cut);
        cutStart = cut;
      }
    }
  }
  if (out.empty())
    return std::nullopt;
  return out;
}

} // namespace sfs::nav
