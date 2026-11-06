#include "engine/runtime/navigation/vehicle/vehicleSearch.h"

#include <algorithm>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace sfs::nav::vehicle
{

namespace
{
constexpr float kUnknown = 1.0e5f; // (a cell the 2D heuristic can't reach)

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
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
          const glm::vec2 c = origin + (glm::vec2(x, y) + 0.5f) * cell;
          float best = 1e9f;
          for (std::size_t i = 1; i < corridor.size(); ++i)
          {
            const glm::vec2 a = corridor[i - 1], d = corridor[i] - a;
            const float t =
                glm::dot(d, d) > 0
                    ? std::clamp(
                          glm::dot(c - a, d) / glm::dot(d, d), 0.0f, 1.0f)
                    : 0.0f;
            best = std::min(best, glm::length(a + t * d - c));
          }
          inside[static_cast<std::size_t>(y) * w + x] = best <= corridorWidth;
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
    std::vector<float> factor;
    if (rig.cost)
      rig.cost->raster(origin, cell, w, h, rig.q.mode, factor);
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
    buildHeading(rig, goals, factor);
  }

  // The heading-aware part: seconds to the goal over coarser cells and 8
  // directions of travel, from cell to cell along the direction (forward at
  // top speed, backing at reverse speed x its penalty) and turning 45
  // degrees in a cell costing what turning that much takes at the tightest
  // radius beyond driving the arc's length flat out (theta (sqrt(R / a) -
  // R / v)). It knows a rig facing away from the goal, or about to come at it
  // from the wrong side, has turning to do; the goal is reached facing its
  // way. A coarser cell is free if any cell in it is (optimistic); where it
  // finds no way it says nothing (the plain 2D part stands). Its cells are the
  // 2D part's (dirCell: a multiple of them).
  static constexpr int kDirs = 8;
  float dirCell = 0.5f;
  int dw = 0, dh = 0;
  glm::vec2 dorigin{0.0f};
  std::vector<float> dsecs; // (cell * kDirs + dir)
  float headingAt(Pose2 p) const
  {
    if (dsecs.empty())
      return 0.0f;
    const glm::ivec2 c =
        glm::ivec2(glm::floor((p.position - dorigin) / dirCell));
    if (c.x < 0 || c.y < 0 || c.x >= dw || c.y >= dh)
      return 0.0f;
    const std::size_t base = (static_cast<std::size_t>(c.y) * dw + c.x) * kDirs;
    // (Between two direction bins: the lesser, so it stays optimistic.)
    const float u = wrapPositive(p.heading) / (kTwoPi / kDirs);
    const int d0 = static_cast<int>(std::floor(u)) % kDirs;
    const int d1 = (d0 + 1) % kDirs;
    const float v = std::min(dsecs[base + d0], dsecs[base + d1]);
    return v >= kUnknown ? 0.0f : v;
  }
  void buildHeading(const Motions& rig,
                    const std::vector<Pose2>& goals,
                    const std::vector<float>& factor)
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
    std::vector<uint8_t> free(cells, 0);
    std::vector<float> least(cells, kInf);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
      {
        const std::size_t i = static_cast<std::size_t>(y) * w + x;
        if (solid[i] || !inside[i])
          continue;
        const std::size_t j = static_cast<std::size_t>(y / r) * dw + x / r;
        free[j] = 1;
        if (std::isfinite(factor[i]))
          least[j] = std::min(least[j], factor[i]);
      }
    const float top = std::max(v.topSpeed, 0.1f);
    const float back = rig.forwardOnly ? 0.0f
                                       : std::max(v.reverseSpeed, 0.05f) /
                                             rig.q.tunables.reversePenalty;
    const float lat = std::max(v.lateralAccel, 0.01f);
    const float perRadian =
        std::max(0.0f, std::sqrt(rig.minRadius / lat) - rig.minRadius / top);
    const float turnCost = perRadian * kTwoPi / kDirs;
    static const int ddx[kDirs] = {1, 1, 0, -1, -1, -1, 0, 1};
    static const int ddy[kDirs] = {0, 1, 1, 1, 0, -1, -1, -1};
    dsecs.assign(cells * kDirs, kUnknown);
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> q;
    for (const Pose2& g : goals)
    {
      const glm::ivec2 c =
          glm::ivec2(glm::floor((g.position - dorigin) / dirCell));
      if (c.x < 0 || c.y < 0 || c.x >= dw || c.y >= dh)
        continue;
      const float u = wrapPositive(g.heading) / (kTwoPi / kDirs);
      const int d0 = static_cast<int>(std::floor(u)) % kDirs;
      for (int d : {d0, (d0 + 1) % kDirs})
      {
        const uint32_t i = static_cast<uint32_t>(
            (static_cast<std::size_t>(c.y) * dw + c.x) * kDirs + d);
        dsecs[i] = 0.0f;
        q.push({0.0f, i});
      }
    }
    // (From the goal back: what reaches (cell, dir) in one move.)
    while (!q.empty())
    {
      const auto [t, i] = q.top();
      q.pop();
      if (t > dsecs[i])
        continue;
      const int d = static_cast<int>(i % kDirs);
      const std::size_t cellIndex = i / kDirs;
      const int x = static_cast<int>(cellIndex % dw);
      const int y = static_cast<int>(cellIndex / dw);
      const auto relax = [&](int nx, int ny, int nd, float cost)
      {
        if (nx < 0 || ny < 0 || nx >= dw || ny >= dh)
          return;
        const std::size_t c = static_cast<std::size_t>(ny) * dw + nx;
        if (!free[c])
          return;
        const uint32_t j = static_cast<uint32_t>(c * kDirs + nd);
        if (t + cost < dsecs[j])
        {
          dsecs[j] = t + cost;
          q.push({t + cost, j});
        }
      };
      const float len = (d % 2 ? 1.41421356f : 1.0f) * dirCell;
      const float f = std::isfinite(least[cellIndex]) ? least[cellIndex] : 1.0f;
      // Driven forward from the cell behind, backing from the one ahead.
      relax(x - ddx[d], y - ddy[d], d, len * f / top);
      if (back > 0.0f)
        relax(x + ddx[d], y + ddy[d], d, len * f / back);
      relax(x, y, (d + 1) % kDirs, turnCost);
      relax(x, y, (d + kDirs - 1) % kDirs, turnCost);
    }
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
  const Tunables& t = rig.q.tunables;
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
      float dt = ds / rig.speedFor(seg.gear, km, dkds);
      if (seg.gear < 0)
        dt *= t.reversePenalty;
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
  int32_t closest =
      0; // (the node with the least heuristic: the clear way so far)
  // (Scratch for grow(), and the near-goal words' times by fine bin.)
  std::vector<Pose2> m_poses;
  std::vector<float> m_hitches;
  std::unordered_map<uint64_t, float> m_near;
  std::unordered_set<uint64_t> m_shotFrom;

  explicit HybridAStar(const SearchContext& ctx)
      : rig(ctx.motions), q(ctx.motions.q), area(ctx.area)
  {
    const std::vector<glm::vec2>& corridor = ctx.corridor;
    std::vector<Pose2> seeds;
    for (const Goal& g : q.goals)
      seeds.push_back(g.pose);
    heur.build(rig, area, seeds, corridor, q.corridorWidth);
    prims = primitivesFor(rig);
    vias = ctx.vias;
    viaDone.assign(vias.size(), 0);
    Node root;
    root.pose = q.from;
    root.hitch = q.hitch;
    root.escape = rig.standsClear(q.from, q.hitch) ? 0.0f : kEscapeReach;
    root.h = hOf(root.pose);
    nodes.push_back(root);
    open.push({root.h, 0.0f, 0});
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
      float dt = sm.dt;
      if (rig.cost)
      {
        const float factor = rig.cost->factor(at, q.mode);
        if (!std::isfinite(factor))
          return false;
        dt *= factor;
      }
      w.cost += dt;
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
    w.end = m_poses.back();
    w.end.heading = wrapAngle(w.end.heading);
    return true;
  }
  float hOf(Pose2 p)
  {
    float h = std::max(heur.at(p.position), heur.headingAt(p));
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
        Steering::Words w;
        w.reverse = !rig.forwardOnly;
        w.cusps = !rig.forwardOnly;
        w.cuspSamples = 0; // (the three-point turns, not the C|CSCs)
        for (const auto& word : rig.levels[0].paths(p, g.pose, w))
          least = std::min(least, rig.estimate(word));
        m_near.emplace(key, least);
      }
      if (std::isfinite(least))
        h = std::max(h, least);
    }
    return h * q.tunables.searchWeight;
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
      for (const auto& word : rig.words(n.pose, vias[v], false))
      {
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
        extra.push_back(word);
        c.h = hOf(c.pose);
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
  bool tryShots(uint32_t from)
  {
    const Node& n = nodes[from];
    // (Once per search bin: another pose in the same bin shoots the same.)
    if (!m_shotFrom
             .insert(keyOf(n, q.tunables, q.vehicle.trailer.has_value(), false))
             .second)
      return false;
    for (std::size_t gi = 0; gi < q.goals.size(); ++gi)
    {
      const Goal& g = q.goals[gi];
      int tried = 0;
      // (Words with cusps only close to the goal: further off the search
      // turns round on its own, and they're most of the words' cost.)
      const bool close =
          glm::length(g.pose.position - n.pose.position) < 4.0f * rig.minRadius;
      for (const auto& word : rig.words(n.pose, g.pose, close, 2))
      {
        if (tried++ >= 3)
          break;
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
        found_ = static_cast<int32_t>(from);
        shot = word;
        goal = static_cast<int>(gi);
        foundHitch = w.hitch;
        return true;
      }
    }
    return false;
  }
  Status run(uint32_t budget) override
  {
    const Tunables& t = q.tunables;
    const bool towing = q.vehicle.trailer.has_value();
    if (m_grown == 0 && nodes.size() == 1 && !q.goals.empty() && tryShots(0))
      return Status::Found;
    while (!open.empty())
    {
      if (m_grown >= budget)
        return Status::Budget;
      if (q.cancel && q.cancel->load(std::memory_order_relaxed))
        return Status::Budget;
      const auto [f, g, i] = open.top();
      open.pop();
      if (g > nodes[i].g)
        continue; // (stale)
      ++m_grown;
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
        if (!hopeless && m_grown % every == 0 && tryShots(i))
          return Status::Found;
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
        c.h = hOf(c.pose);
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
    return Status::Exhausted;
  }
  Way found() const override { return chainTo(found_, true); }
  Way bestSoFar() const override { return chainTo(closest, false); }
  uint32_t grown() const override { return m_grown; }
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
void addHybridAStar(Searches& searches)
{
  searches.add("hybridAStar",
               [](const SearchContext& c)
               { return std::make_unique<HybridAStar>(c); });
}

} // namespace sfs::nav::vehicle
