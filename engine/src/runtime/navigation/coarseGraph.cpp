#include "engine/runtime/navigation/coarseGraph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace sfs::nav
{

namespace
{
constexpr float kFar = 1.0e12f;

// Squared distances along one row (Felzenszwalb and Huttenlocher, "Distance
// Transforms of Sampled Functions", 2012): f in, d out, n long; kFar for
// "nothing there".
void edt1(const float* f,
          float* d,
          int n,
          std::vector<int>& v,
          std::vector<double>& z)
{
  v.assign(static_cast<std::size_t>(n), 0);
  z.assign(static_cast<std::size_t>(n) + 1, 0.0);
  const auto F = [&](int q) { return static_cast<double>(f[q]); };
  int k = 0;
  v[0] = 0;
  z[0] = -1e30;
  z[1] = 1e30;
  for (int q = 1; q < n; ++q)
  {
    double s;
    for (;;)
    {
      const int p = v[static_cast<std::size_t>(k)];
      s = ((F(q) + double(q) * q) - (F(p) + double(p) * p)) / (2.0 * (q - p));
      if (s <= z[static_cast<std::size_t>(k)] && k > 0)
        --k;
      else
        break;
    }
    if (s <= z[0] && k == 0)
    {
      v[0] = q;
      z[0] = -1e30;
      z[1] = 1e30;
      continue;
    }
    ++k;
    v[static_cast<std::size_t>(k)] = q;
    z[static_cast<std::size_t>(k)] = s;
    z[static_cast<std::size_t>(k) + 1] = 1e30;
  }
  k = 0;
  for (int q = 0; q < n; ++q)
  {
    while (z[static_cast<std::size_t>(k) + 1] < q)
      ++k;
    const int p = v[static_cast<std::size_t>(k)];
    d[q] = static_cast<float>(
        std::min(double(kFar), double(q - p) * (q - p) + F(p)));
  }
}
} // namespace

std::shared_ptr<CoarseGraph>
CoarseGraph::build(const Space& space, Rect area, Options options)
{
  auto g = std::make_shared<CoarseGraph>();
  g->m_options = options;
  g->m_area = area;
  const float c = std::max(options.cell, 0.05f);
  g->m_w =
      std::max(1, static_cast<int>(std::ceil((area.hi.x - area.lo.x) / c)));
  g->m_h =
      std::max(1, static_cast<int>(std::ceil((area.hi.y - area.lo.y) / c)));
  const int w = g->m_w, h = g->m_h;
  std::vector<uint8_t> solid;
  // (Every cell a solid touches: thin rails and posts never slip between
  // cell middles.)
  space.raster(area.lo, c, w, h, options.rasterInflate + 0.71f * c, solid);
  const std::size_t n =
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  // Squared distance (in cells) to the nearest solid cell: by columns, then
  // by rows (an exact Euclidean distance transform).
  std::vector<float> f(n), col(static_cast<std::size_t>(std::max(w, h))),
      out(static_cast<std::size_t>(std::max(w, h)));
  std::vector<int> v;
  std::vector<double> z;
  for (std::size_t i = 0; i < n; ++i)
    f[i] = solid[i] ? 0.0f : kFar;
  for (int x = 0; x < w; ++x)
  {
    for (int y = 0; y < h; ++y)
      col[static_cast<std::size_t>(y)] = f[static_cast<std::size_t>(y) * w + x];
    edt1(col.data(), out.data(), h, v, z);
    for (int y = 0; y < h; ++y)
      f[static_cast<std::size_t>(y) * w + x] = out[static_cast<std::size_t>(y)];
  }
  for (int y = 0; y < h; ++y)
  {
    float* row = &f[static_cast<std::size_t>(y) * w];
    std::copy(row, row + w, col.begin());
    edt1(col.data(), row, w, v, z);
  }
  // Clearance in metres: to the nearest touched cell's middle (a solid's
  // surface lies within 0.71 cells of it, either way).
  g->m_clear.resize(n);
  for (std::size_t i = 0; i < n; ++i)
    g->m_clear[i] = f[i] >= kFar ? 1.0e6f : std::sqrt(f[i]) * c;
  g->m_factor.assign(n, 1.0f);
  return g;
}

void CoarseGraph::findNarrows(const std::vector<int>& cells,
                              float halfWidth,
                              float tight,
                              std::vector<Pose2>& out) const
{
  const float c = m_options.cell;
  const auto at = [&](std::size_t k)
  {
    const int i = cells[k];
    return m_area.lo + (glm::vec2(static_cast<float>(i % m_w),
                                  static_cast<float>(i / m_w)) +
                        0.5f) *
                           c;
  };
  const std::size_t n = cells.size();
  const std::size_t span =
      std::max<std::size_t>(1, static_cast<std::size_t>(1.5f / c));
  const std::size_t every =
      std::max<std::size_t>(1, static_cast<std::size_t>(5.0f / c));
  const auto clearAt = [&](std::size_t k)
  { return m_clear[static_cast<std::size_t>(cells[k])]; };
  // A pose in the gap's middle at cell k: along the way's normal there to
  // where the clearance peaks, facing along the way.
  const auto place = [&](std::size_t k)
  {
    const glm::vec2 d =
        at(std::min(k + span, n - 1)) - at(k >= span ? k - span : 0);
    if (glm::dot(d, d) < 1e-6f)
      return;
    const float heading = std::atan2(d.y, d.x);
    const glm::vec2 nrm{-std::sin(heading), std::cos(heading)};
    float best = -1.0f, lo = 0.0f, hi = 0.0f;
    for (float o = -1.5f; o <= 1.5f + 1e-4f; o += 0.05f)
    {
      const float v = clearance(at(k) + nrm * o);
      if (v > best + 1e-4f)
      {
        best = v;
        lo = hi = o;
      }
      else if (v >= best - 1e-4f && o - hi <= 0.051f)
        hi = o;
    }
    if (best >= halfWidth - 0.71f * c)
      out.push_back({at(k) + nrm * (0.5f * (lo + hi)), heading});
  };
  // Each tight stretch: one at its tightest, then every few metres out
  // from there both ways (the ends of the way left out: start and goal
  // are the search's own).
  std::size_t k = span;
  while (k + span < n)
  {
    if (clearAt(k) - halfWidth > tight)
    {
      ++k;
      continue;
    }
    std::size_t end = k;
    while (end + 1 + span < n && clearAt(end + 1) - halfWidth <= tight)
      ++end;
    std::size_t least = k;
    for (std::size_t j = k; j <= end; ++j)
      if (clearAt(j) < clearAt(least))
        least = j;
    std::vector<std::size_t> at_;
    for (std::size_t j = least; j >= k + every && j >= every; j -= every)
      at_.push_back(j - every);
    std::reverse(at_.begin(), at_.end());
    at_.push_back(least);
    for (std::size_t j = least + every; j <= end; j += every)
      at_.push_back(j);
    for (std::size_t j : at_)
      place(j);
    k = end + 1;
  }
}

int CoarseGraph::index(glm::vec2 p) const
{
  const glm::ivec2 c = glm::ivec2(glm::floor((p - m_area.lo) / m_options.cell));
  if (c.x < 0 || c.y < 0 || c.x >= m_w || c.y >= m_h)
    return -1;
  return c.y * m_w + c.x;
}

float CoarseGraph::clearance(glm::vec2 p) const
{
  const int i = index(p);
  return i < 0 ? 0.0f : m_clear[static_cast<std::size_t>(i)];
}

void CoarseGraph::addEdge(glm::vec2 a, glm::vec2 b, float factor, float width)
{
  const float c = m_options.cell;
  const glm::vec2 lo = glm::min(a, b) - width, hi = glm::max(a, b) + width;
  const glm::vec2 d = b - a;
  const float dd = std::max(glm::dot(d, d), 1e-6f);
  for (float y = lo.y; y <= hi.y; y += c)
    for (float x = lo.x; x <= hi.x; x += c)
    {
      const glm::vec2 p{x, y};
      const int i = index(p);
      if (i < 0)
        continue;
      const float t = std::clamp(glm::dot(p - a, d) / dd, 0.0f, 1.0f);
      if (glm::length(a + t * d - p) <= 0.5f * width)
        m_factor[static_cast<std::size_t>(i)] =
            std::min(m_factor[static_cast<std::size_t>(i)], factor);
    }
}

std::vector<glm::vec2> CoarseGraph::route(glm::vec2 from,
                                          glm::vec2 to,
                                          float halfWidth,
                                          std::vector<Pose2>* narrows,
                                          float tight) const
{
  const float c = m_options.cell;
  // (A little optimistic (the grid tells clearance to within 0.7 of a
  // cell): a corridor is a guide, never the check.)
  const float need = std::max(0.0f, halfWidth - 0.35f * c);
  const auto open = [&](int i)
  { return m_clear[static_cast<std::size_t>(i)] >= need; };
  // The ends: the nearest open cell within a few metres (a start beside a
  // wall, in a shed).
  const auto snap = [&](glm::vec2 p) -> int
  {
    const int i0 = index(p);
    if (i0 < 0)
      return -1;
    if (open(i0))
      return i0;
    const int r = static_cast<int>(std::ceil(4.0f / c));
    const int x0 = i0 % m_w, y0 = i0 / m_w;
    int best = -1;
    int bestD = std::numeric_limits<int>::max();
    for (int dy = -r; dy <= r; ++dy)
      for (int dx = -r; dx <= r; ++dx)
      {
        const int x = x0 + dx, y = y0 + dy;
        if (x < 0 || y < 0 || x >= m_w || y >= m_h)
          continue;
        const int d = dx * dx + dy * dy;
        if (d < bestD && open(y * m_w + x))
        {
          bestD = d;
          best = y * m_w + x;
        }
      }
    return best;
  };
  const int s = snap(from), t = snap(to);
  if (s < 0 || t < 0)
    return {};
  float least = 1.0f;
  for (float f : m_factor)
    least = std::min(least, f);
  const int tx = t % m_w, ty = t / m_w;
  const auto hOf = [&](int i)
  {
    const float dx = static_cast<float>(std::abs(i % m_w - tx));
    const float dy = static_cast<float>(std::abs(i / m_w - ty));
    return (std::max(dx, dy) + 0.41421356f * std::min(dx, dy)) * c * least;
  };
  const std::size_t n = m_clear.size();
  std::vector<float> g(n, kFar);
  std::vector<int> parent(n, -1);
  using Item = std::pair<float, int>;
  std::priority_queue<Item, std::vector<Item>, std::greater<>> q;
  g[static_cast<std::size_t>(s)] = 0.0f;
  q.push({hOf(s), s});
  static const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
  static const int dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
  const float pref = std::max(m_options.preferOpen, 1e-3f);
  bool reached = false;
  while (!q.empty())
  {
    const auto [f, i] = q.top();
    q.pop();
    const float gi = g[static_cast<std::size_t>(i)];
    if (f > gi + hOf(i) + 1e-4f)
      continue;
    if (i == t)
    {
      reached = true;
      break;
    }
    const int x = i % m_w, y = i / m_w;
    for (int k = 0; k < 8; ++k)
    {
      const int nx = x + dx[k], ny = y + dy[k];
      if (nx < 0 || ny < 0 || nx >= m_w || ny >= m_h)
        continue;
      const int j = ny * m_w + nx;
      if (!open(j))
        continue;
      // (Keeping off the walls where there's room: dearer near them.)
      const float spare = m_clear[static_cast<std::size_t>(j)] - halfWidth;
      const float near = 1.0f + m_options.openCost *
                                    std::clamp(1.0f - spare / pref, 0.0f, 1.0f);
      const float nd = gi + (k < 4 ? 1.0f : 1.41421356f) * c * near *
                                m_factor[static_cast<std::size_t>(j)];
      if (nd < g[static_cast<std::size_t>(j)])
      {
        g[static_cast<std::size_t>(j)] = nd;
        parent[static_cast<std::size_t>(j)] = i;
        q.push({nd + hOf(j), j});
      }
    }
  }
  if (!reached)
    return {};
  std::vector<int> cells;
  for (int i = t; i >= 0; i = parent[static_cast<std::size_t>(i)])
    cells.push_back(i);
  std::reverse(cells.begin(), cells.end());
  const auto at = [&](int i)
  {
    return m_area.lo + (glm::vec2(static_cast<float>(i % m_w),
                                  static_cast<float>(i / m_w)) +
                        0.5f) *
                           c;
  };
  if (narrows)
    findNarrows(cells, halfWidth, tight, *narrows);
  // Pulled straight where the ground in between is open (fewer corners).
  const auto sees = [&](glm::vec2 a, glm::vec2 b)
  {
    const int steps =
        static_cast<int>(std::ceil(glm::length(b - a) / (0.5f * c))) + 1;
    // (Over ground of one price: a game's track isn't cut across.)
    const int i0 = index(a);
    const float f0 = i0 < 0 ? 1.0f : m_factor[static_cast<std::size_t>(i0)];
    for (int k = 0; k <= steps; ++k)
    {
      const int i = index(a + (b - a) * (static_cast<float>(k) / steps));
      if (i < 0 || !open(i) || m_factor[static_cast<std::size_t>(i)] != f0)
        return false;
    }
    return true;
  };
  std::vector<glm::vec2> out{from};
  std::size_t k = 0;
  while (k + 1 < cells.size())
  {
    std::size_t far = k + 1;
    const glm::vec2 a = at(cells[k]);
    // (Furthest cell seen, looked for in growing steps, then back.)
    std::size_t step = 1;
    while (far + step < cells.size() && sees(a, at(cells[far + step])))
    {
      far += step;
      step *= 2;
    }
    while (step > 1)
    {
      step /= 2;
      if (far + step < cells.size() && sees(a, at(cells[far + step])))
        far += step;
    }
    out.push_back(at(cells[far]));
    k = far;
  }
  out.back() = to;
  return out;
}

} // namespace sfs::nav
