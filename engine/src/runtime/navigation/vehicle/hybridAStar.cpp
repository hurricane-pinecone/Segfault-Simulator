#include "engine/runtime/navigation/vehicle/vehicleSearch.h"

#include "engine/core/util/threadPriority.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <queue>
#include <span>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace sfs::nav::vehicle
{

namespace
{
// (where the time goes: PlanStats)
struct ProfTimes
{
  double heur = 0, shot = 0, heading = 0, words = 0, near = 0;
};
thread_local ProfTimes t_prof;
struct ProfScope
{
  double& into;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  explicit ProfScope(double& d) : into(d) {}
  ~ProfScope()
  {
    into += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0)
                .count();
  }
};
constexpr float kUnknown = 1.0e5f; // (a cell the 2D heuristic can't reach)
// (Heading maps made on threads of their own at once, at most: past that a
// search makes its own when it needs it, as before. The planning pool takes
// the machine's threads less two (the game's and the render's): a thread per
// search on top of it would crowd the game's frame out.)
constexpr int kHeadingHelpers = 1;
std::atomic<int> headingHelpers{0};
// (Poses grown before the heading-aware heuristic is built.)
constexpr uint32_t kInformAfter = 300;

// The 2D heuristic: seconds to the nearest goal round what's solid (walls
// pushed out by the vehicle's inscribed half width), at top speed over the
// cost layer's least factors: never more than the real time.
struct Heuristic
{
  glm::vec2 origin{0.0f};
  float cell = 0.5f;
  int w = 0, h = 0;
  std::vector<float> secs;
  std::vector<uint8_t> inside; // (in the area searched, the corridor)
  std::vector<uint8_t> solid;  // (the reference point can't be there)
  std::vector<uint8_t> open;   // (in the open: nothing solid near)
  bool none = false;           // (a goal region: no heuristic)

  bool contains(glm::vec2 p) const
  {
    const glm::ivec2 c = glm::ivec2(glm::floor((p - origin) / cell));
    return c.x >= 0 && c.y >= 0 && c.x < w && c.y < h &&
           inside[static_cast<std::size_t>(c.y) * w + c.x];
  }
  // Whether the reference point there certainly touches (a solid within
  // the rig's inscribed half width): a word through it needn't be checked.
  bool blocked(glm::vec2 p) const
  {
    if (solid.empty())
      return false;
    const glm::ivec2 c = glm::ivec2(glm::floor((p - origin) / cell));
    return c.x >= 0 && c.y >= 0 && c.x < w && c.y < h &&
           solid[static_cast<std::size_t>(c.y) * w + c.x];
  }
  // Whether nothing solid is within `openClear` of p (the search's bins
  // may be coarser there).
  bool isOpen(glm::vec2 p) const
  {
    if (open.empty())
      return false;
    const glm::ivec2 c = glm::ivec2(glm::floor((p - origin) / cell));
    return c.x >= 0 && c.y >= 0 && c.x < w && c.y < h &&
           open[static_cast<std::size_t>(c.y) * w + c.x];
  }
  float at(glm::vec2 p) const
  {
    if (none)
      return 0.0f;
    const glm::ivec2 c = glm::ivec2(glm::floor((p - origin) / cell));
    if (c.x < 0 || c.y < 0 || c.x >= w || c.y >= h)
      return kUnknown;
    return secs[static_cast<std::size_t>(c.y) * w + c.x];
  }
  void build(const Motions& rig,
             Rect area,
             const std::vector<Pose2>& goals,
             const std::vector<glm::vec2>& corridor,
             float corridorWidth)
  {
    cell = rig.q.tunables.heuristicCell;
    origin = area.lo;
    w = std::max(
        1, static_cast<int>(std::ceil((area.hi.x - area.lo.x) / cell)));
    h = std::max(
        1, static_cast<int>(std::ceil((area.hi.y - area.lo.y) / cell)));
    const std::size_t n =
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    inside.assign(n, 1);
    if (corridor.size() >= 2)
    {
      // (A cell's in if any stretch of the corridor is within its width:
      // each stretch's own cells looked at, those round its box, never the
      // whole area's for every stretch.)
      inside.assign(n, 0);
      for (std::size_t i = 1; i < corridor.size(); ++i)
      {
        const glm::vec2 a = corridor[i - 1], d = corridor[i] - a;
        const glm::vec2 lo =
            (glm::min(a, corridor[i]) - corridorWidth - origin) / cell - 1.0f;
        const glm::vec2 hi =
            (glm::max(a, corridor[i]) + corridorWidth - origin) / cell + 1.0f;
        const int x0 = std::max(0, static_cast<int>(std::floor(lo.x)));
        const int y0 = std::max(0, static_cast<int>(std::floor(lo.y)));
        const int x1 = std::min(w - 1, static_cast<int>(std::ceil(hi.x)));
        const int y1 = std::min(h - 1, static_cast<int>(std::ceil(hi.y)));
        for (int y = y0; y <= y1; ++y)
          for (int x = x0; x <= x1; ++x)
          {
            uint8_t& in = inside[static_cast<std::size_t>(y) * w + x];
            if (in)
              continue;
            const glm::vec2 c = origin + (glm::vec2(x, y) + 0.5f) * cell;
            const float t =
                glm::dot(d, d) > 0
                    ? std::clamp(
                          glm::dot(c - a, d) / glm::dot(d, d), 0.0f, 1.0f)
                    : 0.0f;
            in = glm::length(a + t * d - c) <= corridorWidth;
          }
      }
    }
    secs.assign(n, kUnknown);
    solid.clear();
    std::vector<glm::vec2> seeds;
    for (const Pose2& g : goals)
      seeds.push_back(g.position);
    if (seeds.empty())
    {
      none = true;
      return;
    }
    rig.space.raster(origin,
                     cell,
                     w,
                     h,
                     std::max(rig.inscribed - 0.71f * cell, 0.0f),
                     solid);
    // The open: cells with no solid within openClear (a chamfer distance
    // from the solids, both ways over the raster).
    {
      const float openClear = rig.q.tunables.openClear;
      std::vector<float> d(n, 1e9f);
      for (std::size_t i = 0; i < n; ++i)
        if (solid[i])
          d[i] = 0.0f;
      const float c1 = cell, c2 = 1.41421356f * cell;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
          float& v = d[static_cast<std::size_t>(y) * w + x];
          if (x > 0)
            v = std::min(v, d[static_cast<std::size_t>(y) * w + x - 1] + c1);
          if (y > 0)
          {
            const std::size_t up = static_cast<std::size_t>(y - 1) * w + x;
            v = std::min(v, d[up] + c1);
            if (x > 0)
              v = std::min(v, d[up - 1] + c2);
            if (x + 1 < w)
              v = std::min(v, d[up + 1] + c2);
          }
        }
      for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x)
        {
          float& v = d[static_cast<std::size_t>(y) * w + x];
          if (x + 1 < w)
            v = std::min(v, d[static_cast<std::size_t>(y) * w + x + 1] + c1);
          if (y + 1 < h)
          {
            const std::size_t dn = static_cast<std::size_t>(y + 1) * w + x;
            v = std::min(v, d[dn] + c1);
            if (x > 0)
              v = std::min(v, d[dn - 1] + c2);
            if (x + 1 < w)
              v = std::min(v, d[dn + 1] + c2);
          }
        }
      const float reach =
          std::max(rig.reachX, rig.reachY) +
          (rig.q.vehicle.trailer ? rig.q.vehicle.trailer->hitchBehind +
                                       rig.q.vehicle.trailer->drawbar + rig.towX
                                 : 0.0f);
      open.assign(n, 0);
      if (openClear > 0.0f)
        for (std::size_t i = 0; i < n; ++i)
          open[i] = d[i] > reach + openClear;
    }
    factor.clear();
    if (rig.cost)
    {
      const auto t0 = std::chrono::steady_clock::now();
      rig.cost->raster(origin, cell, w, h, rig.q.mode, factor);
      CheckStats& stats = checkStats();
      ++stats.costs;
      stats.costMs += std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    }
    else
      factor.assign(n, 1.0f);
    const float speed = std::max(rig.q.vehicle.topSpeed, 0.1f);
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    for (const glm::vec2 s : seeds)
    {
      const glm::ivec2 c = glm::ivec2(glm::floor((s - origin) / cell));
      if (c.x < 0 || c.y < 0 || c.x >= w || c.y >= h)
        continue;
      const uint32_t i = static_cast<uint32_t>(c.y * w + c.x);
      secs[i] = 0.0f;
      open.push({0.0f, i});
    }
    static const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (!open.empty())
    {
      const auto [d, i] = open.top();
      open.pop();
      if (d > secs[i])
        continue;
      const int x = static_cast<int>(i) % w, y = static_cast<int>(i) / w;
      for (int k = 0; k < 8; ++k)
      {
        const int nx = x + dx[k], ny = y + dy[k];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h)
          continue;
        const uint32_t j = static_cast<uint32_t>(ny * w + nx);
        if (solid[j] || !inside[j])
          continue;
        const float len = (k < 4 ? 1.0f : 1.41421356f) * cell;
        const float f = std::min(factor[i], factor[j]);
        const float nd = d + len * (std::isfinite(f) ? f : 1.0f) / speed;
        if (nd < secs[j])
        {
          secs[j] = nd;
          open.push({nd, j});
        }
      }
    }
    buildTurning(rig, goals, factor);
  }

  // The turning part, built with the 2D part (cheap): seconds to the goal over
  // coarser cells and 8 directions of travel, from cell to cell along the
  // direction (forward at top speed, backing at reverse speed x its penalty)
  // and turning 45 degrees in a cell costing what turning that much takes at
  // the tightest radius beyond driving the arc's length flat out (theta (sqrt(R
  // / a) - R / v)). It knows a rig facing away from the goal, or about to come
  // at it from the wrong side, has turning to do; the goal is reached facing
  // its way. A coarser cell is free if any cell in it is (optimistic); where it
  // finds no way it says nothing (the plain 2D part stands). Its cells are the
  // 2D part's (turnCell: a multiple of them).
  static constexpr int kTurnDirs = 8;
  float turnCell = 0.5f;
  int tw = 0, th = 0;
  glm::vec2 torigin{0.0f};
  std::vector<float> tsecs; // (cell * kTurnDirs + dir)
  float turningAt(Pose2 p) const
  {
    if (tsecs.empty())
      return 0.0f;
    const glm::ivec2 c =
        glm::ivec2(glm::floor((p.position - torigin) / turnCell));
    if (c.x < 0 || c.y < 0 || c.x >= tw || c.y >= th)
      return 0.0f;
    const std::size_t base =
        (static_cast<std::size_t>(c.y) * tw + c.x) * kTurnDirs;
    // (Between two direction bins: the lesser, so it stays optimistic.)
    const float u = wrapPositive(p.heading) / (kTwoPi / kTurnDirs);
    const int d0 = static_cast<int>(std::floor(u)) % kTurnDirs;
    const int d1 = (d0 + 1) % kTurnDirs;
    const float v = std::min(tsecs[base + d0], tsecs[base + d1]);
    return v >= kUnknown ? 0.0f : v;
  }
  void buildTurning(const Motions& rig,
                    const std::vector<Pose2>& goals,
                    const std::vector<float>& factor)
  {
    tsecs.clear();
    if (goals.empty())
      return;
    const VehicleModel& v = rig.q.vehicle;
    const int r = std::max(1, static_cast<int>(std::lround(turnCell / cell)));
    turnCell = cell * static_cast<float>(r);
    torigin = origin;
    tw = (w + r - 1) / r;
    th = (h + r - 1) / r;
    const std::size_t cells = static_cast<std::size_t>(tw) * th;
    std::vector<uint8_t> free(cells, 0);
    std::vector<float> least(cells, kInf);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
      {
        const std::size_t i = static_cast<std::size_t>(y) * w + x;
        if (solid[i] || !inside[i])
          continue;
        const std::size_t j = static_cast<std::size_t>(y / r) * tw + x / r;
        free[j] = 1;
        if (std::isfinite(factor[i]))
          least[j] = std::min(least[j], factor[i]);
      }
    const float top = std::max(v.topSpeed, 0.1f);
    const float back = rig.forwardOnly ? 0.0f : std::max(v.reverseSpeed, 0.05f);
    const float lat = std::max(v.lateralAccel, 0.01f);
    const float perRadian =
        std::max(0.0f, std::sqrt(rig.minRadius / lat) - rig.minRadius / top);
    const float turnCost = perRadian * kTwoPi / kTurnDirs;
    static const int ddx[kTurnDirs] = {1, 1, 0, -1, -1, -1, 0, 1};
    static const int ddy[kTurnDirs] = {0, 1, 1, 1, 0, -1, -1, -1};
    tsecs.assign(cells * kTurnDirs, kUnknown);
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> q;
    for (const Pose2& g : goals)
    {
      const glm::ivec2 c =
          glm::ivec2(glm::floor((g.position - torigin) / turnCell));
      if (c.x < 0 || c.y < 0 || c.x >= tw || c.y >= th)
        continue;
      const float u = wrapPositive(g.heading) / (kTwoPi / kTurnDirs);
      const int d0 = static_cast<int>(std::floor(u)) % kTurnDirs;
      for (int d : {d0, (d0 + 1) % kTurnDirs})
      {
        const uint32_t i = static_cast<uint32_t>(
            (static_cast<std::size_t>(c.y) * tw + c.x) * kTurnDirs + d);
        tsecs[i] = 0.0f;
        q.push({0.0f, i});
      }
    }
    // (From the goal back: what reaches (cell, dir) in one move.)
    while (!q.empty())
    {
      const auto [t, i] = q.top();
      q.pop();
      if (t > tsecs[i])
        continue;
      const int d = static_cast<int>(i % kTurnDirs);
      const std::size_t cellIndex = i / kTurnDirs;
      const int x = static_cast<int>(cellIndex % tw);
      const int y = static_cast<int>(cellIndex / tw);
      const auto relax = [&](int nx, int ny, int nd, float cost)
      {
        if (nx < 0 || ny < 0 || nx >= tw || ny >= th)
          return;
        const std::size_t c = static_cast<std::size_t>(ny) * tw + nx;
        if (!free[c])
          return;
        const uint32_t j = static_cast<uint32_t>(c * kTurnDirs + nd);
        if (t + cost < tsecs[j])
        {
          tsecs[j] = t + cost;
          q.push({t + cost, j});
        }
      };
      const float len = (d % 2 ? 1.41421356f : 1.0f) * turnCell;
      const float f = std::isfinite(least[cellIndex]) ? least[cellIndex] : 1.0f;
      // Driven forward from the cell behind, backing from the one ahead.
      relax(x - ddx[d], y - ddy[d], d, len * f / top);
      if (back > 0.0f)
        relax(x + ddx[d], y + ddy[d], d, len * f / back);
      relax(x, y, (d + 1) % kTurnDirs, turnCost);
      relax(x, y, (d + kTurnDirs - 1) % kTurnDirs, turnCost);
    }
  }

  // The heading-aware part (the non-holonomic heuristic, with obstacles):
  // seconds to the goal by (cell, heading, gear), found from the goal back
  // over the rig's own motions as a point that turns no tighter than its
  // radius: short straights and full-lock arcs, forward (and backing, if it
  // may, a cusp at each change) at the speeds it takes them, its reference
  // point kept out of what's solid (the 2D part's raster). It is hybrid A*
  // run backwards as a Dijkstra over coarse bins (dirCell x kDirs): each bin
  // keeps the pose that reached it cheapest and goes on from there. It knows
  // what Dolgov et al.'s two heuristics know apart (a rig facing away from
  // its goal has turning to do; the way round what's solid) and what neither
  // knows: that turning round takes room, so where there's none the way is
  // round. It stops once past `reach` seconds (well beyond the start's): a
  // bin it didn't reach is at least that far.
  static constexpr int kDirs = 16;
  float dirCell = 1.0f;
  int dw = 0, dh = 0;
  glm::vec2 dorigin{0.0f};
  std::vector<float> dsecs; // ((cell * kDirs + dir) * 2 + gear: 0 forward)
  float cusp = 0.0f;
  float beyond = 0.0f; // (a bin not reached: at least this)
  // (Whether it got near the start (two turning radii: a start hemmed in
  // tighter than its coarse motions go), and whether it ran into the edge
  // of the area searched on the way: if it didn't and did, the way is
  // likely outside it.)
  bool nearStart = false, metEdge = false, reachedStart = false;
  // (`gear`: the one it drives in now, 0 standing: a change costs a cusp.)
  float headingAt(Pose2 p, int gear = 0) const
  {
    if (dsecs.empty())
      return turningAt(p); // (not built yet)
    const glm::ivec2 c =
        glm::ivec2(glm::floor((p.position - dorigin) / dirCell));
    if (c.x < 0 || c.y < 0 || c.x >= dw || c.y >= dh)
      return 0.0f;
    const std::size_t base = (static_cast<std::size_t>(c.y) * dw + c.x) * kDirs;
    // (Between two direction bins: the lesser, so it stays optimistic.)
    const float u = wrapPositive(p.heading) / (kTwoPi / kDirs) - 0.5f;
    const int d0 = (static_cast<int>(std::floor(u)) + kDirs) % kDirs;
    const int d1 = (d0 + 1) % kDirs;
    float v = kUnknown;
    for (const int d : {d0, d1})
      for (int g = 0; g < 2; ++g)
      {
        const bool change = gear != 0 && (gear > 0) != (g == 0);
        v = std::min(v, dsecs[(base + d) * 2 + g] + (change ? cusp : 0.0f));
      }
    return v >= kUnknown ? beyond : v;
  }
  std::vector<float> factor; // (the cost layer's, by cell)
  // A copy of what the heading-aware part is made from (the 2D part's
  // raster and cells), to make it on another thread.
  Heuristic headingInputs() const
  {
    Heuristic h;
    h.origin = origin;
    h.cell = cell;
    h.w = w;
    h.h = this->h;
    h.inside = inside;
    h.solid = solid;
    h.factor = factor;
    return h;
  }
  // The heading-aware part made by another (headingInputs()'s copy).
  void takeHeading(Heuristic&& from)
  {
    dirCell = from.dirCell;
    dw = from.dw;
    dh = from.dh;
    dorigin = from.dorigin;
    dsecs = std::move(from.dsecs);
    cusp = from.cusp;
    beyond = from.beyond;
    nearStart = from.nearStart;
    metEdge = from.metEdge;
    reachedStart = from.reachedStart;
  }
  // (`stop`: given up part way when it turns true: no map, never used.)
  void buildHeading(const Motions& rig,
                    const std::vector<Pose2>& goals,
                    Pose2 start,
                    const std::atomic<bool>* stop = nullptr)
  {
    dsecs.clear();
    if (goals.empty())
      return;
    const VehicleModel& v = rig.q.vehicle;
    const int r = std::max(1, static_cast<int>(std::lround(dirCell / cell)));
    dirCell = cell * static_cast<float>(r);
    dorigin = origin;
    dw = (w + r - 1) / r;
    dh = (h + r - 1) / r;
    const std::size_t cells = static_cast<std::size_t>(dw) * dh;
    std::vector<float> least(cells, kInf);
    // (free: the body may be there; 2: the reference point may too, the
    // ground not barred to it (an infinite factor: the search never goes
    // there).)
    std::vector<uint8_t> free(static_cast<std::size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
      {
        const std::size_t i = static_cast<std::size_t>(y) * w + x;
        if (solid[i] || !inside[i])
          continue;
        free[i] = 1;
        if (!std::isfinite(factor[i]))
          continue;
        free[i] = 2;
        const std::size_t j = static_cast<std::size_t>(y / r) * dw + x / r;
        least[j] = std::min(least[j], factor[i]);
      }
    // (Hot: most of the map's time. Off the low side is off the raster, so
    // a plain truncation does for the floor.)
    const auto cellOf = [&](glm::vec2 p, float size, int cw, int ch)
    {
      const float fx = (p.x - origin.x) / size, fy = (p.y - origin.y) / size;
      if (!(fx >= 0.0f && fy >= 0.0f))
        return int64_t{-1};
      const int x = static_cast<int>(fx), y = static_cast<int>(fy);
      return x >= cw || y >= ch ? int64_t{-1}
                                : static_cast<int64_t>(y) * cw + x;
    };
    const float dirWidth = kTwoPi / kDirs;
    const auto binOf = [&](Pose2 p) -> int64_t
    {
      const int64_t c = cellOf(p.position, dirCell, dw, dh);
      if (c < 0)
        return -1;
      const int d =
          static_cast<int>(std::floor(wrapPositive(p.heading) / dirWidth)) %
          kDirs;
      return c * kDirs + d;
    };
    // The motions, driven backwards from where they end (what reaches a
    // pose by each): straight a little longer than a bin's diagonal, so each
    // leaves its bin, and at full lock either way. Each laid out
    // once in its end's frame: its samples and its start.
    // (Turning at the search's own rate: its turn primitives straighten
    // out at each end, so a run of them turns no tighter than one does on
    // average (in and out of lock over its length), not at full lock.)
    float kmax = 1.0f / std::max(rig.minRadius, 0.01f);
    if (!rig.levels.empty())
    {
      const Steering& st = rig.levels[0];
      const float arc = std::max(0.0f, 2.0f - 2.0f * st.clothoidLength());
      const float length = 2.0f * st.clothoidLength() + arc;
      if (length > 0.0f)
        kmax = std::min(
            kmax, (2.0f * st.clothoidTurn() + arc * st.kappa()) / length);
    }
    struct Move
    {
      int gear;
      float secs;                 // (on ground of factor 1)
      std::vector<glm::vec2> at;  // (samples; the last is its start)
      std::vector<glm::vec2> dir; // (the heading at each)
      float turn;                 // (its start's heading, from the end's)
      float length = 0.0f;
      float reach = 0.0f; // (its discs' furthest from where it's driven to)
    };
    // The rig's body as discs of the inscribed radius along it, its
    // reference point's and as far ahead and behind as the boxes reach:
    // none of them may be on what's solid either.
    std::vector<float> discs{0.0f};
    for (const Box3& b : rig.boxes)
    {
      if (std::abs(b.shift) > b.half - rig.inscribed + 1e-3f)
        continue; // (a box off to one side: the discs stay on the middle)
      if (b.front - rig.inscribed > 0.5f * cell)
        discs.push_back(b.front - rig.inscribed);
      if (b.back + rig.inscribed < -0.5f * cell)
        discs.push_back(b.back + rig.inscribed);
    }
    // And its outline: the boxes' edges, a point at most a cell apart,
    // shrunk by half a cell's diagonal, over a raster of the cells anything
    // solid touches (thin fences too): a point in one may be about that
    // far clear, so it stays about as optimistic. A trailer's in its
    // own frame, held at the angle it settles to on the move's turn (in
    // line on a straight): the corners the discs miss, and what it sweeps.
    const float shrink = 0.71f * cell;
    const auto outline = [&](const Box3& b, std::vector<glm::vec2>& out)
    {
      const float x0 = b.back + shrink, x1 = std::max(x0, b.front - shrink);
      const float y0 = b.shift - b.half + shrink,
                  y1 = std::max(y0, b.shift + b.half - shrink);
      const int nx = std::max(1, static_cast<int>(std::ceil((x1 - x0) / cell)));
      const int ny = std::max(1, static_cast<int>(std::ceil((y1 - y0) / cell)));
      for (int i = 0; i <= nx; ++i)
      {
        const float x = x0 + (x1 - x0) * static_cast<float>(i) / nx;
        out.push_back({x, y0});
        out.push_back({x, y1});
      }
      for (int i = 1; i < ny; ++i)
      {
        const float y = y0 + (y1 - y0) * static_cast<float>(i) / ny;
        out.push_back({x0, y});
        out.push_back({x1, y});
      }
    };
    std::vector<glm::vec2> body, towed;
    for (const Box3& b : rig.boxes)
      outline(b, body);
    const Trailer* tr = v.trailer ? &*v.trailer : nullptr;
    if (tr)
      outline(tr->box, towed);
    const auto stopped = [&]
    {
      if (!stop || !stop->load(std::memory_order_relaxed))
        return false;
      dsecs.clear();
      return true;
    };
    std::vector<uint8_t> edge; // (the cells anything solid touches)
    rig.space.raster(origin, cell, w, h, shrink, edge);
    if (stopped())
      return;
    // (How far each cell is from the nearest one the outline could be
    // stopped by (solid, or ground barred under it), in metres, kept on the
    // low side (an 8-way chamfer overstates by up to 8.3%, and a cell's
    // width either end): where an outline's whole circle is further off than
    // that, none of its points can be, so they aren't looked at. Open ground,
    // most of a map, is passed at a glance; the answer is the same.)
    std::vector<float> room(static_cast<std::size_t>(w) * h, 1e9f);
    for (std::size_t i = 0; i < room.size(); ++i)
      if (edge[i] || !std::isfinite(factor[i]))
        room[i] = 0.0f;
    {
      const float dg = 1.41421356f;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
          float& r = room[static_cast<std::size_t>(y) * w + x];
          if (x > 0)
            r = std::min(r, room[static_cast<std::size_t>(y) * w + x - 1] + 1);
          if (y > 0)
          {
            const std::size_t up = static_cast<std::size_t>(y - 1) * w + x;
            r = std::min(r, room[up] + 1);
            if (x > 0)
              r = std::min(r, room[up - 1] + dg);
            if (x + 1 < w)
              r = std::min(r, room[up + 1] + dg);
          }
        }
      for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x)
        {
          float& r = room[static_cast<std::size_t>(y) * w + x];
          if (x + 1 < w)
            r = std::min(r, room[static_cast<std::size_t>(y) * w + x + 1] + 1);
          if (y + 1 < h)
          {
            const std::size_t dn = static_cast<std::size_t>(y + 1) * w + x;
            r = std::min(r, room[dn] + 1);
            if (x > 0)
              r = std::min(r, room[dn - 1] + dg);
            if (x + 1 < w)
              r = std::min(r, room[dn + 1] + dg);
          }
        }
      for (float& r : room)
        r = r * cell / 1.0824f - 2.0f * cell;
    }
    if (stopped())
      return;
    const auto roomAt = [&](glm::vec2 at)
    {
      const int64_t c = cellOf(at, cell, w, h);
      return c < 0 ? -1.0f : room[static_cast<std::size_t>(c)];
    };
    // (Likewise how far each cell is from the nearest one a disc can't be on
    // (not free, or ground barred to the reference point) or the raster's
    // edge, kept on the low side: a move whose discs all stay within that of
    // where it starts can't meet one, so they aren't looked at one by one.)
    std::vector<float> discRoom(static_cast<std::size_t>(w) * h, 1e9f);
    {
      const float dg = 1.41421356f;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
          const std::size_t i = static_cast<std::size_t>(y) * w + x;
          float& r = discRoom[i];
          if (free[i] < 2)
            r = 0.0f;
          r = std::min(r,
                       static_cast<float>(std::min(
                           std::min(x, w - 1 - x), std::min(y, h - 1 - y))));
          if (x > 0)
            r = std::min(r, discRoom[i - 1] + 1);
          if (y > 0)
          {
            const std::size_t up = i - static_cast<std::size_t>(w);
            r = std::min(r, discRoom[up] + 1);
            if (x > 0)
              r = std::min(r, discRoom[up - 1] + dg);
            if (x + 1 < w)
              r = std::min(r, discRoom[up + 1] + dg);
          }
        }
      for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x)
        {
          const std::size_t i = static_cast<std::size_t>(y) * w + x;
          float& r = discRoom[i];
          if (x + 1 < w)
            r = std::min(r, discRoom[i + 1] + 1);
          if (y + 1 < h)
          {
            const std::size_t dn = i + static_cast<std::size_t>(w);
            r = std::min(r, discRoom[dn] + 1);
            if (x > 0)
              r = std::min(r, discRoom[dn - 1] + dg);
            if (x + 1 < w)
              r = std::min(r, discRoom[dn + 1] + dg);
          }
        }
      for (float& r : discRoom)
        r = r * cell / 1.0824f - 2.0f * cell;
    }
    const auto discRoomAt = [&](glm::vec2 at)
    {
      const int64_t c = cellOf(at, cell, w, h);
      return c < 0 ? -1.0f : discRoom[static_cast<std::size_t>(c)];
    };
    // (Each outline's circle: about a centre, in its own frame.)
    const auto circle = [](const std::vector<glm::vec2>& pts)
    {
      glm::vec2 lo(1e9f), hi(-1e9f);
      for (const glm::vec2 p : pts)
      {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
      }
      const glm::vec2 c = pts.empty() ? glm::vec2(0.0f) : 0.5f * (lo + hi);
      float r = 0.0f;
      for (const glm::vec2 p : pts)
        r = std::max(r, glm::length(p - c));
      return std::array<float, 3>{c.x, c.y, r};
    };
    const std::array<float, 3> bodyRing = circle(body), towRing = circle(towed);
    const auto settled = [&](float k, float length)
    {
      float a = 0.0f;
      if (tr && k != 0.0f)
        a = hitchStep(*tr, 0.0f, length, k);
      return tr ? std::clamp(a, -tr->forwardLimit, tr->forwardLimit) : 0.0f;
    };
    std::vector<Move> moves;
    std::vector<glm::vec2> hitchTurn; // (by move: cos, sin of its angle)
    for (int gear : {1, -1})
    {
      if (gear < 0 && rig.forwardOnly)
        continue;
      for (float k : {0.0f, kmax, -kmax})
      {
        float speed = gear > 0 ? v.topSpeed : v.reverseSpeed;
        if (k != 0.0f)
          speed = std::min(speed, bendSpeed(v, kmax));
        // (An arc turns a whole direction bin: a smaller turn would lose,
        // bin by bin, to the straighter ways into the same bins.)
        const float step = k != 0.0f ? std::max(1.5f * dirCell, dirWidth / kmax)
                                     : 1.5f * dirCell;
        const int samples =
            std::max(1, static_cast<int>(std::ceil(step / cell)));
        Move m{gear, step / std::max(speed, 0.05f), {}, {}, 0.0f};
        const Segment back{-gear, step, k, k};
        for (int s = 1; s <= samples; ++s)
        {
          const Pose2 a = advance(
              {},
              back,
              step * static_cast<float>(s) / static_cast<float>(samples));
          m.at.push_back(a.position);
          m.dir.push_back(unit(a.heading));
        }
        m.turn = advance({}, back, step).heading;
        m.length = step;
        for (std::size_t k = 0; k < m.at.size(); ++k)
          for (const float along : discs)
            m.reach =
                std::max(m.reach, glm::length(m.at[k] + m.dir[k] * along));
        moves.push_back(std::move(m));
        const float a = settled(k, step);
        hitchTurn.push_back({std::cos(a), std::sin(a)});
      }
    }
    cusp = rig.forwardOnly ? 0.0f : v.cuspSeconds(rig.q.tunables.turnSpeed);
    dsecs.assign(cells * kDirs * 2, kUnknown);
    std::vector<Pose2> at(cells * kDirs * 2);
    // (Metres driven to the goal from each: a trailer's checked only once
    // it's as far out as it reaches back, its angle near the goal being
    // whatever the way in made it.)
    std::vector<float> run(tr ? cells * kDirs * 2 : 0, 0.0f);
    const float towReach =
        tr ? tr->hitchBehind + tr->drawbar - std::min(tr->box.back, 0.0f)
           : 0.0f;
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> q;
    for (std::size_t gi = 0; gi < goals.size(); ++gi)
    {
      const Pose2& g = goals[gi];
      const int64_t b = binOf(g);
      if (b < 0)
        continue;
      // (A goal to be come to in one gear: only that gear's bins seeded.)
      const int arrive =
          gi < rig.q.goals.size() ? rig.q.goals[gi].arriveGear : 0;
      for (int k = 0; k < 2; ++k)
      {
        if (arrive != 0 && (k == 1) != (arrive < 0))
          continue;
        const std::size_t i = static_cast<std::size_t>(b) * 2 + k;
        dsecs[i] = 0.0f;
        at[i] = g;
        q.push({0.0f, static_cast<uint32_t>(i)});
      }
    }
    // (Where to stop: well past the start, once it's reached facing its
    // way (the two direction bins headingAt() reads there); a bin not
    // reached by then is at least that far. Never reached: everything
    // the area holds was, and a bin not reached has no way in it.)
    const int64_t startCell = cellOf(start.position, dirCell, dw, dh);
    const float su = wrapPositive(start.heading) / dirWidth - 0.5f;
    const int sd0 = (static_cast<int>(std::floor(su)) + kDirs) % kDirs;
    const int sd1 = (sd0 + 1) % kDirs;
    float reach = kInf, furthest = 0.0f;
    beyond = 0.0f;
    reachedStart = false;
    nearStart = false;
    metEdge = false;
    const float near = 2.0f * rig.minRadius + dirCell;
    uint32_t pops = 0;
    while (!q.empty())
    {
      if ((++pops & 1023u) == 0 && stopped())
        return;
      const auto [t, i] = q.top();
      q.pop();
      if (t > dsecs[i])
        continue;
      if (t > reach)
      {
        beyond = reach;
        break;
      }
      furthest = t;
      const int dir = static_cast<int>(i / 2 % kDirs);
      if (!reachedStart && static_cast<int64_t>(i / 2 / kDirs) == startCell &&
          (dir == sd0 || dir == sd1))
      {
        reachedStart = true;
        reach = 1.5f * t + 10.0f;
      }
      const Pose2 p = at[i];
      nearStart = nearStart || glm::length(p.position - start.position) <= near;
      const glm::vec2 f = unit(p.heading), l{-f.y, f.x};
      const float fp = least[i / 2 / kDirs];
      const int gear = i % 2 ? -1 : 1; // (what it drives on from p in)
      const float spare = discRoomAt(p.position);
      const auto hits = [&](glm::vec2 at)
      {
        const int64_t c = cellOf(at, cell, w, h);
        return c >= 0 && edge[static_cast<std::size_t>(c)];
      };
      for (std::size_t mi = 0; mi < moves.size(); ++mi)
      {
        const Move& m = moves[mi];
        bool clear = true;
        for (std::size_t k = 0; k < m.at.size() && clear; ++k)
        {
          const glm::vec2 o = m.at[k];
          const glm::vec2 a = p.position + f * o.x + l * o.y;
          const glm::vec2 d = f * m.dir[k].x + l * m.dir[k].y;
          const glm::vec2 n{-d.y, d.x};
          // (The outline at the move's end only (its start, driven): the
          // discs, at every sample, catch what it would pass over.)
          const bool ends = k + 1 == m.at.size();
          bool barredUnder = false; // (ground barred to the reference point)
          const bool lookBody = ends && roomAt(a + d * bodyRing[0] +
                                               n * bodyRing[1]) <= bodyRing[2];
          for (const glm::vec2 b : lookBody ? std::span<const glm::vec2>(body)
                                            : std::span<const glm::vec2>())
          {
            const int64_t c = cellOf(a + d * b.x + n * b.y, cell, w, h);
            if (c < 0)
              continue;
            if (edge[static_cast<std::size_t>(c)])
            {
              clear = false;
              break;
            }
            barredUnder = barredUnder ||
                          !std::isfinite(factor[static_cast<std::size_t>(c)]);
          }
          // (The body over it: the cost layer may bar the whole pose (a body
          // kept off a crop); it says, for this pose.)
          if (clear && barredUnder && rig.cost &&
              !std::isfinite(
                  priceAt(*rig.cost, {a, std::atan2(d.y, d.x)}, rig.q.mode)))
            clear = false;
          if (clear && ends && tr && run[i] >= towReach)
          {
            const glm::vec2 hc = hitchTurn[mi];
            const glm::vec2 td{
                d.x * hc.x + d.y * hc.y, d.y * hc.x - d.x * hc.y};
            const glm::vec2 tn{-td.y, td.x};
            const glm::vec2 axle = a - d * tr->hitchBehind - td * tr->drawbar;
            const bool lookTow =
                roomAt(axle + td * towRing[0] + tn * towRing[1]) <= towRing[2];
            for (const glm::vec2 b : lookTow ? std::span<const glm::vec2>(towed)
                                             : std::span<const glm::vec2>())
              if (hits(axle + td * b.x + tn * b.y))
              {
                clear = false;
                break;
              }
          }
          if (!clear)
            break;
          if (spare > m.reach)
            continue; // (every disc of it in the open)
          for (const float along : discs)
          {
            const int64_t c = cellOf(a + d * along, cell, w, h);
            if (c < 0 ||
                free[static_cast<std::size_t>(c)] < (along == 0.0f ? 2 : 1))
            {
              metEdge =
                  metEdge || c < 0 || !inside[static_cast<std::size_t>(c)];
              clear = false;
              break;
            }
          }
        }
        if (!clear)
          continue;
        const glm::vec2 e = m.at.back();
        const Pose2 from{p.position + f * e.x + l * e.y, p.heading + m.turn};
        const int64_t b = binOf(from);
        if (b < 0)
          continue;
        const std::size_t j = static_cast<std::size_t>(b) * 2 + (m.gear < 0);
        const float fc =
            std::min(fp, least[static_cast<std::size_t>(b) / kDirs]);
        const float nt = t + m.secs * (std::isfinite(fc) ? fc : 1.0f) +
                         (m.gear != gear ? cusp : 0.0f);
        if (nt < dsecs[j])
        {
          dsecs[j] = nt;
          at[j] = from;
          if (tr)
            run[j] = run[i] + m.length;
          q.push({nt, static_cast<uint32_t>(j)});
        }
      }
    }
    if (beyond == 0.0f)
      beyond = furthest + 10.0f; // (all of it searched)
  }
};

// One motion primitive: a few metres of a CC turn or a straight, in a gear,
// with its samples worked out once in its own frame (from the origin facing
// +X): growing a node then turns and moves them (one sine and cosine per
// node, none per sample) instead of integrating the turn again.
struct Primitive
{
  std::vector<Segment> segs;
  struct Sample
  {
    glm::vec2 p{0.0f}; // where, in the primitive's frame
    float turn = 0.0f; // heading there
    float k = 0.0f;    // curvature over the step to it (its middle)
    float ds = 0.0f;   // signed metres of the step (- backing)
    float dt = 0.0f;   // seconds the step takes on ground of factor 1
  };
  std::vector<Sample> samples;
  // (Each segment's last sample + 1, and the bend it looks over with.)
  std::vector<std::size_t> ends;
  std::vector<float> bend;
  int gear = 1;
};

// Fill a primitive's samples as Motions::walk() lays them at `step`.
void tabulate(const Motions& rig, Primitive& prim, float step)
{
  Pose2 at;
  prim.gear = prim.segs.empty() ? 1 : prim.segs.front().gear;
  for (const Segment& seg : prim.segs)
  {
    if (seg.length <= 0.0f)
      continue;
    const int n =
        std::max(1, static_cast<int>(std::ceil(seg.length / step - 1e-4f)));
    const float ds = seg.length / static_cast<float>(n);
    const float dkds = (seg.k1 - seg.k0) / seg.length;
    for (int i = 0; i < n; ++i)
    {
      const float k0 = seg.k0 + dkds * ds * static_cast<float>(i);
      const float k1 = k0 + dkds * ds;
      const float km = 0.5f * (k0 + k1);
      at = advance(at, {seg.gear, ds, k0, k1}, ds);
      const float dt = ds / rig.speedFor(seg.gear, km, dkds);
      prim.samples.push_back(
          {at.position, at.heading, km, static_cast<float>(seg.gear) * ds, dt});
    }
    prim.ends.push_back(prim.samples.size());
    prim.bend.push_back(std::max(std::abs(seg.k0), std::abs(seg.k1)));
  }
}

std::vector<Primitive> primitivesFor(const Motions& rig)
{
  std::vector<Primitive> out;
  const float straight = 1.5f;
  for (int gear : {1, -1})
  {
    if (gear < 0 && rig.forwardOnly)
      continue;
    out.push_back({{{gear, straight, 0.0f, 0.0f}}});
    for (std::size_t li = 0; li < 2; ++li)
    {
      const Steering& st = rig.levels[li];
      // (About two metres: the clothoids in and out, an arc to make it up.)
      const float arc = std::max(0.0f, 2.0f - 2.0f * st.clothoidLength());
      const float deflection = 2.0f * st.clothoidTurn() + arc * st.kappa();
      for (int sense : {1, -1})
      {
        Primitive p;
        if (st.turn(deflection, sense, gear, true, true, p.segs))
          out.push_back(std::move(p));
      }
    }
  }
  for (Primitive& p : out)
    tabulate(rig, p, rig.q.tunables.checkStep);
  return out;
}

struct Node
{
  Pose2 pose;
  float hitch = 0.0f;
  int gear = 0;
  float g = 0.0f, h = 0.0f;
  float escape = 0.0f;
  int32_t parent = -1;
  int32_t prim = -1; // (-1: the root)
};

// A node's search bin: Tunables::cell and headingBin, both doubled in the
// open (`open`: no solid within reach + openClear), where nothing needs
// lining up finely and the fine bins only multiply the poses grown.
uint64_t keyOf(const Node& n, const Tunables& t, bool towing, bool open)
{
  const float cell = open ? 2.0f * t.cell : t.cell;
  const float bin = open ? 2.0f * t.headingBin : t.headingBin;
  const int64_t ix =
      static_cast<int64_t>(std::floor(n.pose.position.x / cell)) + (1 << 19);
  const int64_t iy =
      static_cast<int64_t>(std::floor(n.pose.position.y / cell)) + (1 << 19);
  const int64_t ih =
      static_cast<int64_t>(std::floor(wrapPositive(n.pose.heading) / bin)) +
      (open ? 64 : 0);
  const int64_t hb =
      towing ? static_cast<int64_t>(std::lround(n.hitch / t.hitchBin)) + 15 : 0;
  return (static_cast<uint64_t>(ix & 0xFFFFF) << 44) |
         (static_cast<uint64_t>(iy & 0xFFFFF) << 24) |
         (static_cast<uint64_t>(ih & 0x7F) << 17) |
         (static_cast<uint64_t>(hb & 0x1F) << 12) | (n.gear > 0 ? 1u : 0u);
}

// The search, resumable: run() grows poses up to a budget and stops; out of
// budget, run() again with more goes on where it stopped.
class HybridAStar : public ISearch
{
public:
  const Motions& rig;
  const PlanRequest& q;
  Rect area;
  Heuristic heur;
  std::vector<Primitive> prims;
  std::vector<std::vector<Segment>> extra; // (shots to the vias: prim -2-k)
  std::vector<Pose2> vias;
  std::vector<uint8_t> viaDone;
  std::vector<Node> nodes;
  using Item = std::tuple<float, float, uint32_t>; // (f, g, index)
  std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
  std::unordered_map<uint64_t, uint32_t> best;
  uint32_t m_grown = 0, m_shots = 0;
  bool m_leftArea = false;
  int32_t found_ = -1;
  std::vector<Segment> shot; // (the found one's last stretch)
  int goal = -1;
  float foundHitch = 0.0f;
  // (The quickest way to a goal a shot has found so far, all told: taken
  // once no pose still open could beat it (its f), never just the first; or
  // once Tunables::improveNodes more have been grown looking for one.)
  float m_shotCost = kInf;
  uint32_t m_shotAt = 0; // (poses grown when the first shot found a way)
  uint32_t m_bestAt = 0, m_ways = 0; // (when the best was found; how many)
  float m_firstCost = kInf;
  int32_t closest =
      0; // (the node with the least heuristic: the clear way so far)
  // (Scratch for grow(), and the near-goal words' times by fine bin.)
  std::vector<Pose2> m_poses;
  std::vector<float> m_hitches;
  std::unordered_map<uint64_t, float> m_near;
  std::unordered_set<uint64_t> m_shotFrom;
  std::vector<glm::vec2> corridor;
  bool m_ready = false;
  bool m_informed = false;
  bool m_outside = false; // (inform(): the way's likely outside the area)
  // The heading-aware map, made on a thread of its own from the moment the
  // search starts (it needs nothing the search does): ready, or nearly, by
  // the time the search has grown kInformAfter poses, instead of the search
  // standing while it's made. The same map: the same search.
  struct Heading
  {
    Heuristic map;
    std::atomic<bool> stop{false};
    bool made = false;
    double ms = 0.0;
    std::thread worker;
  };
  std::unique_ptr<Heading> m_heading;

  explicit HybridAStar(const SearchContext& ctx)
      : rig(ctx.motions), q(ctx.motions.q), area(ctx.area),
        corridor(ctx.corridor)
  {
    vias = ctx.vias;
    viaDone.assign(vias.size(), 0);
    Node root;
    root.pose = q.from;
    root.hitch = q.hitch;
    root.escape = rig.standsClear(q.from, q.hitch) ? 0.0f : kEscapeReach;
    nodes.push_back(root);
  }
  ~HybridAStar() override
  {
    if (m_heading && m_heading->worker.joinable())
    {
      m_heading->stop = true;
      m_heading->worker.join();
    }
  }
  HybridAStar(const HybridAStar&) = delete;
  HybridAStar& operator=(const HybridAStar&) = delete;
  // The heuristic and the primitives, built only once a search is needed:
  // a way the first shot finds (a plain way) costs neither.
  void ready()
  {
    if (m_ready)
      return;
    m_ready = true;
    const ProfScope prof(t_prof.heur);
    std::vector<Pose2> seeds;
    for (const Goal& g : q.goals)
      seeds.push_back(g.pose);
    heur.build(rig, area, seeds, corridor, q.corridorWidth);
    if (!seeds.empty() && headingHelpers.fetch_add(1) < kHeadingHelpers)
    {
      m_heading = std::make_unique<Heading>();
      m_heading->map = heur.headingInputs();
      Heading* job = m_heading.get();
      try
      {
        job->worker = std::thread(
            [job, seeds, this]
            {
              lowerThisThreadPriority(); // (planning: PlanScheduler's)
              const auto t0 = std::chrono::steady_clock::now();
              try
              {
                job->map.buildHeading(rig, seeds, q.from, &job->stop);
                job->made = !job->stop.load();
              }
              catch (...)
              {
                job->made = false; // (made again on the search's thread)
              }
              job->ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
              --headingHelpers;
            });
      }
      catch (...)
      {
        m_heading.reset(); // (no thread to be had: made when it's needed)
        --headingHelpers;
      }
    }
    else if (!seeds.empty())
      --headingHelpers;
    prims = primitivesFor(rig);
    nodes[0].h = hOf(nodes[0].pose);
    open.push({nodes[0].h, 0.0f, 0});
  }
  // The heading-aware heuristic, built once the search has grown
  // kInformAfter poses (a plain way never needs it): every pose waiting is
  // priced again with it.
  void inform()
  {
    m_informed = true;
    const ProfScope prof(t_prof.heur);
    const ProfScope heading(t_prof.heading);
    std::vector<Pose2> seeds;
    for (const Goal& g : q.goals)
      seeds.push_back(g.pose);
    if (m_heading)
    {
      m_heading->worker.join();
      if (m_heading->made)
        heur.takeHeading(std::move(m_heading->map));
      else
        heur.buildHeading(rig, seeds, q.from);
      m_heading.reset();
    }
    else
      heur.buildHeading(rig, seeds, q.from);
    // (Its motions never got from the goals to the start in the area, and
    // ran into the area's edge: the way is likely outside it; on searching
    // here only floods. Done here, as if it had left the area: the planner
    // widens it. Never a proof: the planner says "no way" only when the
    // area is everything there is.)
    if (!heur.nearStart && heur.metEdge)
    {
      // (Not when it's searching all there is already: the planner's
      // widest, everything there is and 30 m round it and the trip.)
      Rect world = rig.space.bounds();
      world.lo = glm::min(world.lo, q.from.position);
      world.hi = glm::max(world.hi, q.from.position);
      for (const Goal& g : q.goals)
      {
        world.lo = glm::min(world.lo, g.pose.position);
        world.hi = glm::max(world.hi, g.pose.position);
      }
      m_outside =
          area.lo.x > world.lo.x - 29.0f || area.lo.y > world.lo.y - 29.0f ||
          area.hi.x < world.hi.x + 29.0f || area.hi.y < world.hi.y + 29.0f;
    }
    std::vector<Item> waiting;
    waiting.reserve(open.size() + 1);
    while (!open.empty())
    {
      waiting.push_back(open.top());
      open.pop();
    }
    for (const auto& [f, g, i] : waiting)
    {
      if (g > nodes[i].g)
        continue;
      Node& n = nodes[i];
      n.h = hOf(n.pose, n.gear);
      open.push({n.g + n.h, n.g, i});
    }
  }
  // Along a primitive from node `n` (facing f, its left l): its poses from
  // the table, priced and checked as Motions::walk() does.
  bool grow(const Node& n,
            glm::vec2 f,
            glm::vec2 l,
            const Primitive& prim,
            Motions::Walk& w)
  {
    const VehicleModel& v = q.vehicle;
    const Tunables& t = q.tunables;
    if (rig.forwardOnly && prim.gear < 0)
      return false;
    if (w.gear != 0 && prim.gear != w.gear)
      w.cost += v.cuspSeconds(t.turnSpeed);
    w.gear = prim.gear;
    m_poses.clear();
    m_hitches.clear();
    m_poses.push_back(n.pose);
    m_hitches.push_back(w.hitch);
    for (const Primitive::Sample& sm : prim.samples)
    {
      const Pose2 at{
          n.pose.position + f * sm.p.x + l * sm.p.y, n.pose.heading + sm.turn};
      if (!rig.cost)
        w.cost += sm.dt;
      if (v.trailer)
      {
        const float was = w.hitch;
        w.hitch = hitchStep(*v.trailer, w.hitch, sm.ds, sm.k);
        const float limit =
            prim.gear > 0 ? v.trailer->forwardLimit : v.trailer->reverseLimit;
        if (std::abs(w.hitch) > limit + w.slack &&
            std::abs(w.hitch) > std::abs(was))
          return false;
      }
      m_poses.push_back(at);
      m_hitches.push_back(w.hitch);
    }
    std::size_t from = 0;
    for (std::size_t sg = 0; sg < prim.ends.size(); ++sg)
    {
      const std::size_t to = prim.ends[sg];
      if (to > from && !rig.clearRange(m_poses,
                                       m_hitches,
                                       prim.bend[sg],
                                       std::abs(prim.samples[to - 1].ds),
                                       from,
                                       to))
        return false;
      from = to;
    }
    // (Priced once it's found clear, in the same order: a motion that
    // touches never asks the cost layer.)
    if (rig.cost)
      for (std::size_t k = 0; k < prim.samples.size(); ++k)
      {
        const float factor = priceAt(*rig.cost, m_poses[k + 1], q.mode);
        if (!std::isfinite(factor))
          return false;
        w.cost += prim.samples[k].dt * factor;
      }
    w.end = m_poses.back();
    w.end.heading = wrapAngle(w.end.heading);
    return true;
  }
  float hOf(Pose2 p, int gear = 0)
  {
    float h = std::max(heur.at(p.position), heur.headingAt(p, gear));
    // (Near a goal its turning matters: the quickest word there, unchecked;
    // remembered per fine bin, as many poses come close to each other.)
    for (std::size_t gi = 0; gi < q.goals.size(); ++gi)
    {
      const Goal& g = q.goals[gi];
      if (glm::length(g.pose.position - p.position) >= 4.0f * rig.minRadius)
        continue;
      const uint64_t key =
          (static_cast<uint64_t>(gi) << 52) ^
          (static_cast<uint64_t>(
               static_cast<int64_t>(std::floor(p.position.x / 0.1f)) & 0xFFFFF)
           << 32) ^
          (static_cast<uint64_t>(
               static_cast<int64_t>(std::floor(p.position.y / 0.1f)) & 0xFFFFF)
           << 12) ^
          static_cast<uint64_t>(std::floor(wrapPositive(p.heading) / 0.01f));
      float least = kInf;
      if (const auto it = m_near.find(key); it != m_near.end())
        least = it->second;
      else
      {
        const ProfScope prof(t_prof.near);
        Steering::Words w;
        w.reverse = !rig.forwardOnly;
        w.cusps = !rig.forwardOnly;
        w.cuspSamples = 0; // (the three-point turns, not the C|CSCs)
        // (Every word, however it arrives: the words with cusps are sampled
        // too sparsely to bound one asked to arrive in a gear.)
        rig.levels[0].paths(p,
                            g.pose,
                            w,
                            [&](std::span<const Segment> word)
                            { least = std::min(least, rig.estimate(word)); });
        m_near.emplace(key, least);
      }
      if (std::isfinite(least))
        h = std::max(h, least);
    }
    return h * q.tunables.searchWeight;
  }
  // (Motions::wordsInto(), timed: PlanStats::wordsMs; into one of the
  // search's own lists, laid out flat.)
  mutable Motions::WordList m_wordLists[2];
  const Motions::WordList&
  timedWords(Pose2 a, Pose2 b, bool cusps, int cuspSamples, int list = 0) const
  {
    const ProfScope prof(t_prof.words);
    Motions::WordList& out = m_wordLists[list];
    rig.wordsInto(a, b, cusps, cuspSamples, out);
    return out;
  }
  // A word worth checking: its reference point never in a cell where it
  // certainly touches (cheap: no boxes, no trailer).
  bool plausible(Pose2 from, std::span<const Segment> word) const
  {
    Pose2 at = from;
    for (const Segment& seg : word)
    {
      if (seg.length <= 0.0f)
        continue;
      const int n = std::max(1, static_cast<int>(std::ceil(seg.length / 0.5f)));
      const float ds = seg.length / static_cast<float>(n);
      const float dkds = (seg.k1 - seg.k0) / seg.length;
      for (int i = 0; i < n; ++i)
      {
        const float k0 = seg.k0 + dkds * ds * static_cast<float>(i);
        at = advance(at, {seg.gear, ds, k0, k0 + dkds * ds}, ds);
        if (heur.blocked(at.position))
          return false;
      }
    }
    return true;
  }
  // Shots at the gaps' middles ahead: a node there, lined up through it.
  void tryVias(uint32_t from)
  {
    const ProfScope prof(t_prof.shot);
    const Node& n = nodes[from];
    const float hn = heur.at(n.pose.position);
    // (Only the next one ahead: the nearest to the goal still behind the
    // node's heuristic, within reach.)
    std::size_t v = vias.size();
    float vh = -1.0f;
    for (std::size_t k = 0; k < vias.size(); ++k)
    {
      const float hk = heur.at(vias[k].position);
      const float d = glm::length(vias[k].position - n.pose.position);
      if (viaDone[k] || hk >= hn || d > 14.0f || d < 1.0f)
        continue;
      if (hk > vh)
      {
        vh = hk;
        v = k;
      }
    }
    if (v < vias.size())
    {
      int tried = 0;
      const Motions::WordList& words = timedWords(n.pose, vias[v], false, 10);
      for (std::size_t wi = 0; wi < words.size(); ++wi)
      {
        const std::span<const Segment> word = words.at(wi);
        if (std::any_of(word.begin(),
                        word.end(),
                        [](const Segment& sg) { return sg.gear < 0; }))
          continue;
        if (tried++ >= 2)
          break;
        if (!plausible(n.pose, word))
          continue;
        ++m_shots;
        Motions::Walk w;
        w.hitch = n.hitch;
        w.gear = n.gear;
        w.escape = n.escape;
        if (!rig.walk(n.pose, word, q.tunables.checkStep, true, w) ||
            !arrives(w.end, vias[v]))
          continue;
        Node c;
        c.pose = vias[v];
        c.hitch = w.hitch;
        c.gear = w.gear;
        c.g = n.g + w.cost;
        c.escape = w.escape;
        c.parent = static_cast<int32_t>(from);
        c.prim = -2 - static_cast<int32_t>(extra.size());
        extra.emplace_back(word.begin(), word.end());
        c.h = hOf(c.pose, c.gear);
        const uint32_t idx = static_cast<uint32_t>(nodes.size());
        nodes.push_back(c);
        best[keyOf(c,
                   q.tunables,
                   q.vehicle.trailer.has_value(),
                   heur.isOpen(c.pose.position))] = idx;
        open.push({c.g + c.h, c.g, idx});
        viaDone[v] = 1;
        break;
      }
    }
  }
  // A shot that gets there: the way found if it's the quickest yet.
  // The time braking to a stand at the end of a word, as the speed profile
  // has it.
  float stopOf(std::span<const Segment> word) const
  {
    for (auto it = word.rbegin(); it != word.rend(); ++it)
      if (it->length > 0.0f)
        return q.vehicle.stopSeconds(
            rig.speedFor(it->gear, it->k1, (it->k1 - it->k0) / it->length));
    return 0.0f;
  }
  // Whether a shot of at least `least` seconds from `from` can't beat the
  // way found (offer() would turn it down): not walked. (A hair over, so the
  // walk's own sum of the same times never comes in under it.)
  bool cantBeat(uint32_t from, float least) const
  {
    return m_shotCost < kInf &&
           nodes[from].g + least > m_shotCost * 1.001f + 1.0e-3f;
  }
  bool offer(uint32_t from,
             std::vector<Segment> word,
             const Motions::Walk& w,
             int gi)
  {
    // (Its time all told: to here, the shot, and braking to a stand at the
    // goal, as the speed profile has it.)
    const float stop = stopOf(word);
    const float total = nodes[from].g + w.cost + stop;
    if (total >= m_shotCost)
      return false;
    if (m_shotCost == kInf)
    {
      m_shotAt = m_grown;
      m_firstCost = total;
    }
    ++m_ways;
    m_bestAt = m_grown;
    m_shotCost = total;
    found_ = static_cast<int32_t>(from);
    shot = std::move(word);
    goal = gi;
    foundHitch = w.hitch;
    return true;
  }
  // Shots from `from` at the goals: each that gets there offered (offer()).
  bool tryShots(uint32_t from)
  {
    const ProfScope prof(t_prof.shot);
    const Node& n = nodes[from];
    // (Once per search bin: another pose in the same bin shoots the same.)
    if (!m_shotFrom
             .insert(keyOf(n, q.tunables, q.vehicle.trailer.has_value(), false))
             .second)
      return false;
    bool offered = false;
    for (std::size_t gi = 0; gi < q.goals.size(); ++gi)
    {
      const Goal& g = q.goals[gi];
      int tried = 0;
      // (Words with cusps only close to the goal: further off the search
      // turns round on its own, and they're most of the words' cost.)
      const bool close =
          glm::length(g.pose.position - n.pose.position) < 4.0f * rig.minRadius;
      // (None of them could beat the way found, straight there flat out:
      // nothing to offer, nothing looked at. The same offers by
      // construction.)
      const bool direct =
          !cantBeat(from, rig.leastTime(n.pose.position, g.pose.position));
      const Motions::WordList* words =
          direct ? &timedWords(n.pose, g.pose, close, 2) : nullptr;
      for (std::size_t wi = 0; words && wi < words->size(); ++wi)
      {
        const std::span<const Segment> word = words->at(wi);
        // (A goal to be come to in one gear: only words ending in it.)
        if (!arrivesAsAsked(g, word, rig.length))
          continue;
        if (tried++ >= 3)
          break;
        if (cantBeat(from, rig.leastTime(word) + stopOf(word)))
          continue; // (offer() would turn it down, walked or not)
        if (!plausible(n.pose, word))
          continue;
        ++m_shots;
        Motions::Walk w;
        w.hitch = n.hitch;
        w.gear = n.gear;
        w.escape = n.escape;
        if (!rig.walk(n.pose, word, q.tunables.checkStep, true, w) ||
            !arrives(w.end, g.pose))
          continue;
        if (g.hitch &&
            std::abs(wrapAngle(w.hitch - *g.hitch)) > g.hitchTolerance)
          continue;
        offered = offer(from,
                        std::vector<Segment>(word.begin(), word.end()),
                        w,
                        static_cast<int>(gi)) ||
                  offered;
      }
      // Its approach (as a driver parks in a bay): a goal hemmed in (a shed,
      // a gap) is reached straight in, the way it faces or backing in, from
      // its lined-up pose a turn or two out in the open; a shot there and the
      // straight on, both walked as any. Without, a goal with room to turn
      // only straight out of it floods the search round it.
      if (!close)
        continue;
      const glm::vec2 f(std::cos(g.pose.heading), std::sin(g.pose.heading));
      // (Asked to come to it in a gear: the straight in at least the rig's
      // length, the whole rig coming in that way.)
      const float out = g.arriveGear != 0
                            ? std::max(2.0f * rig.minRadius, rig.length)
                            : 2.0f * rig.minRadius;
      for (const int gear : {1, -1})
      {
        if ((gear < 0 && rig.forwardOnly) ||
            (g.arriveGear != 0 && gear != g.arriveGear))
          continue;
        // (Forward in: lined up behind it; backing in: ahead of it.)
        const Pose2 lined{
            g.pose.position - f * (out * static_cast<float>(gear)),
            g.pose.heading};
        const Segment in{gear, out, 0.0f, 0.0f};
        // (Not even straight to where it lines up and in could beat the way
        // found: nothing here to offer.)
        if (cantBeat(from,
                     rig.leastTime(n.pose.position, lined.position) +
                         rig.leastTime(std::span<const Segment>(&in, 1)) +
                         stopOf(std::span<const Segment>(&in, 1))))
          continue;
        Motions::Walk last;
        last.gear = gear;
        if (!rig.walk(lined,
                      std::span<const Segment>(&in, 1),
                      q.tunables.checkStep,
                      true,
                      last) ||
            !arrives(last.end, g.pose))
          continue;
        int triedIn = 0;
        const Motions::WordList& inWords =
            timedWords(n.pose, lined, true, 2, 1);
        for (std::size_t wi = 0; wi < inWords.size(); ++wi)
        {
          const std::span<const Segment> word = inWords.at(wi);
          if (triedIn++ >= 3)
            break;
          if (!plausible(n.pose, word))
            continue;
          ++m_shots;
          std::vector<Segment> whole(word.begin(), word.end());
          whole.push_back(in);
          Motions::Walk w;
          w.hitch = n.hitch;
          w.gear = n.gear;
          w.escape = n.escape;
          if (!rig.walk(n.pose, whole, q.tunables.checkStep, true, w) ||
              !arrives(w.end, g.pose))
            continue;
          if (g.hitch &&
              std::abs(wrapAngle(w.hitch - *g.hitch)) > g.hitchTolerance)
            continue;
          offered =
              offer(from, std::move(whole), w, static_cast<int>(gi)) || offered;
          break;
        }
      }
    }
    return offered;
  }
  Status run(uint32_t budget) override
  {
    const Tunables& t = q.tunables;
    const bool towing = q.vehicle.trailer.has_value();
    if (!m_ready && !q.goals.empty())
      tryShots(0);
    ready();
    while (!open.empty())
    {
      if (m_grown >= budget)
        return m_shotCost < kInf ? Status::Found : Status::Budget;
      if (m_shotCost < kInf && m_grown >= m_shotAt + t.improveNodes)
        return Status::Found;
      if (q.cancel && q.cancel->load(std::memory_order_relaxed))
        return Status::Budget;
      const auto [f, g, i] = open.top();
      open.pop();
      if (g > nodes[i].g)
        continue; // (stale)
      // (A shot's found a way: a pose that can't beat it all told, its time
      // so far and at least what's left, is never grown; the way is taken
      // when none is left that could. At least what's left: the heuristic's
      // lower bounds only (the 2D map's and the turning's, unweighted), not
      // its near-goal words, which a manoeuvre of several cusps can beat.)
      if (m_shotCost < kInf &&
          nodes[i].g + std::max(heur.at(nodes[i].pose.position),
                                heur.headingAt(nodes[i].pose, nodes[i].gear)) >=
              m_shotCost)
        continue;
      ++m_grown;
      if (!m_informed && m_grown >= kInformAfter)
      {
        open.push({f, g, i}); // (priced again with the rest)
        inform();
        if (m_outside)
        {
          if (m_shotCost < kInf)
            return Status::Found;
          m_leftArea = true;
          return Status::Exhausted;
        }
        continue;
      }
      const Node n = nodes[i];
      if (n.h < nodes[static_cast<std::size_t>(closest)].h)
        closest = static_cast<int32_t>(i);
      if (q.goalTest && n.escape <= 0.0f && q.goalTest(n.pose))
      {
        found_ = static_cast<int32_t>(i);
        shot.clear();
        foundHitch = n.hitch;
        return Status::Found;
      }
      // (Shots: often near a goal, now and then far off.)
      if (!q.goals.empty())
      {
        float dist = 1e9f;
        for (const Goal& gl : q.goals)
          dist =
              std::min(dist, glm::length(gl.pose.position - n.pose.position));
        const uint32_t every =
            dist < 15.0f
                ? 2u
                : static_cast<uint32_t>(std::max(1.0f, dist / t.goalShotEvery));
        // (Far off and the way round things much longer than straight
        // there: a shot can't make it.)
        const bool hopeless =
            dist >= 15.0f && !heur.none &&
            heur.at(n.pose.position) * q.vehicle.topSpeed > 1.3f * dist + 4.0f;
        if (!hopeless && m_grown % every == 0)
          tryShots(i);
      }
      if (!vias.empty() && m_grown % 3 == 0)
        tryVias(i);
      const glm::vec2 fw = unit(n.pose.heading), lw{-fw.y, fw.x};
      for (std::size_t p = 0; p < prims.size(); ++p)
      {
        Motions::Walk w;
        w.hitch = n.hitch;
        w.gear = n.gear;
        w.escape = n.escape;
        // (Getting out of a touch: pose by pose, the general walk.)
        if (n.escape > 0.0f
                ? !rig.walk(n.pose, prims[p].segs, t.checkStep, true, w)
                : !grow(n, fw, lw, prims[p], w))
          continue;
        Node c;
        c.pose = w.end;
        c.hitch = w.hitch;
        c.gear = w.gear;
        c.g = n.g + w.cost;
        c.escape = w.escape;
        c.parent = static_cast<int32_t>(i);
        c.prim = static_cast<int32_t>(p);
        if (!heur.contains(c.pose.position))
        {
          m_leftArea = true;
          continue;
        }
        c.h = hOf(c.pose, c.gear);
        const uint64_t key = keyOf(c, t, towing, heur.isOpen(c.pose.position));
        const auto it = best.find(key);
        if (it != best.end() && nodes[it->second].g <= c.g)
          continue;
        const uint32_t idx = static_cast<uint32_t>(nodes.size());
        nodes.push_back(c);
        best[key] = idx;
        open.push({c.g + c.h, c.g, idx});
      }
    }
    return m_shotCost < kInf ? Status::Found : Status::Exhausted;
  }
  Way found() const override { return chainTo(found_, true); }
  Way bestSoFar() const override { return chainTo(closest, false); }
  uint32_t grown() const override { return m_grown; }
  void describe(PlanStats& s) const override
  {
    s.ways = m_ways;
    s.firstAt = m_shotAt;
    s.bestAt = m_bestAt;
    s.firstSeconds = m_ways ? m_firstCost : 0.0f;
    s.bestSeconds = m_ways ? m_shotCost : 0.0f;
  }
  uint32_t shots() const override { return m_shots; }
  bool leftArea() const override { return m_leftArea; }
  // What hems it in: what the motions out of the poses nearest the goal
  // (by the heuristic) run into, a handful, each collider once.
  std::vector<Space::Hit> blockers() const override
  {
    std::vector<Space::Hit> out;
    std::vector<uint32_t> order(nodes.size());
    for (uint32_t k = 0; k < order.size(); ++k)
      order[k] = k;
    const std::size_t look = std::min<std::size_t>(order.size(), 12);
    std::partial_sort(
        order.begin(),
        order.begin() + static_cast<std::ptrdiff_t>(look),
        order.end(),
        [&](uint32_t x, uint32_t y)
        { return nodes[x].h != nodes[y].h ? nodes[x].h < nodes[y].h : x < y; });
    const void* body = rig.probe(false, 0.05f, 0.04f);
    const void* tow =
        q.vehicle.trailer ? rig.probe(true, 0.05f, 0.04f) : nullptr;
    for (std::size_t o = 0; o < look && out.size() < 8; ++o)
    {
      const Node& c = nodes[order[o]];
      for (const Primitive& p : prims)
      {
        Pose2 at = c.pose;
        float hitch = c.hitch;
        for (const Segment& seg : p.segs)
        {
          const int n =
              std::max(1, static_cast<int>(std::ceil(seg.length / 0.1f)));
          const float ds = seg.length / static_cast<float>(n);
          const float dkds =
              seg.length > 0 ? (seg.k1 - seg.k0) / seg.length : 0;
          bool hitHere = false;
          for (int i = 0; i < n && !hitHere; ++i)
          {
            const float k0 = seg.k0 + dkds * ds * static_cast<float>(i);
            at = advance(at, {seg.gear, ds, k0, k0 + dkds * ds}, ds);
            if (q.vehicle.trailer)
              hitch = hitchStep(*q.vehicle.trailer,
                                hitch,
                                static_cast<float>(seg.gear) * ds,
                                k0 + 0.5f * dkds * ds);
            std::optional<Space::Hit> h =
                body ? rig.space.hit(body, at) : std::nullopt;
            if (!h && tow)
              h = rig.space.hit(
                  tow, trailerPose(*q.vehicle.trailer, at, hitch));
            if (!h)
              continue;
            hitHere = true;
            if (std::none_of(out.begin(),
                             out.end(),
                             [&](const Space::Hit& x)
                             { return x.id == h->id; }))
              out.push_back(*h);
          }
          if (hitHere)
            break;
        }
      }
    }
    return out;
  }

  // The way to node `i`: the primitives' segments (and the shot, if found).
  Way chainTo(int32_t i, bool withShot) const
  {
    std::vector<int32_t> ids;
    for (int32_t k = i; k >= 0; k = nodes[static_cast<std::size_t>(k)].parent)
      ids.push_back(k);
    std::reverse(ids.begin(), ids.end());
    Way c;
    c.goal = withShot ? goal : -1;
    for (std::size_t k = 0; k < ids.size(); ++k)
    {
      const Node& n = nodes[static_cast<std::size_t>(ids[k])];
      c.poses.push_back(n.pose);
      c.g.push_back(n.g);
      c.hitch.push_back(n.hitch);
      if (k > 0)
        c.links.push_back(n.prim >= 0
                              ? prims[static_cast<std::size_t>(n.prim)].segs
                              : extra[static_cast<std::size_t>(-2 - n.prim)]);
    }
    if (withShot && !shot.empty())
    {
      Motions::Walk w;
      w.hitch = c.hitch.back();
      w.gear = nodes[static_cast<std::size_t>(i)].gear;
      rig.walk(c.poses.back(), shot, q.tunables.checkStep, false, w);
      c.links.push_back(shot);
      c.poses.push_back(q.goals[static_cast<std::size_t>(goal)].pose);
      c.g.push_back(c.g.back() + w.cost);
      c.hitch.push_back(w.hitch);
    }
    return c;
  }
};
} // namespace

// The built-in search (Searches): hybrid A* (Dolgov, Thrun, Montemerlo and
// Diebel, "Path Planning for Autonomous Vehicles in Unknown Semi-structured
// Environments", IJRR 2010) over continuous-curvature motions, with analytic
// continuous-curvature shots to the goal.
void takeProfTimes(double& heur,
                   double& shot,
                   double& heading,
                   double& words,
                   double& near)
{
  heur = t_prof.heur;
  shot = t_prof.shot;
  heading = t_prof.heading;
  words = t_prof.words;
  near = t_prof.near;
  t_prof = {};
}

std::vector<HeuristicParts> heuristicAt(const PlanRequest& request,
                                        std::span<const Pose2> poses)
{
  std::vector<HeuristicParts> out(poses.size());
  if (!request.space || request.goals.empty())
    return out;
  const Motions rig(request);
  // (The planner's first area: round the start and goals, its margin.)
  Rect area{request.from.position, request.from.position};
  std::vector<Pose2> seeds;
  for (const Goal& g : request.goals)
  {
    area.lo = glm::min(area.lo, g.pose.position);
    area.hi = glm::max(area.hi, g.pose.position);
    seeds.push_back(g.pose);
  }
  area.lo -= request.margin;
  area.hi += request.margin;
  Heuristic h;
  h.build(rig, area, seeds, request.corridor, request.corridorWidth);
  h.buildHeading(rig, seeds, request.from);
  for (std::size_t i = 0; i < poses.size(); ++i)
    out[i] = {
        h.at(poses[i].position), h.turningAt(poses[i]), h.headingAt(poses[i])};
  return out;
}

void addHybridAStar(Searches& searches)
{
  searches.add("hybridAStar",
               [](const SearchContext& c)
               { return std::make_unique<HybridAStar>(c); });
}

} // namespace sfs::nav::vehicle
