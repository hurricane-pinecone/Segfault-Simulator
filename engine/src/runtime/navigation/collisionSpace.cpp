#include "engine/runtime/navigation/collisionSpace.h"

#include "engine/core/logger/logger.h"
#include "engine/core/util/debugFeatures.h"
#include "engine/runtime/navigation/coarseGraph.h"

#include "glm/glm/gtc/quaternion.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace sfs::nav
{

namespace
{
glm::vec2 rotate(glm::vec2 v, float a)
{
  const float c = std::cos(a), s = std::sin(a);
  return {c * v.x - s * v.y, s * v.x + c * v.y};
}
} // namespace

namespace
{
// (A query timed into this thread's CheckStats.)
struct Timed
{
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  ~Timed()
  {
    checkStats().ms += std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
  }
};
// (What prepare() hands back: the probe, the span it stands on, and its
// shapes (the broad phase's circles round them).)
struct Prepared
{
  CollisionWorld3D::Probe probe;
  Span span;
  std::vector<CollisionShape3D> shapes;
};

// (A hair more than the broad phase needs clear: Jolt's least half size of a
// shape, 5 mm, and rounding.)
constexpr float kHair = 0.02f;

// How far `p` is from a footprint (m; 0 or less on or in it).
float distanceTo(const BoxSpace::Solid& f, glm::vec2 p)
{
  if (f.circle)
    return glm::length(p - f.centre) - f.half.x;
  const glm::vec2 local = rotate(p - f.centre, -f.yaw);
  return glm::length(local - glm::clamp(local, -f.half, f.half));
}

// How far a footprint reaches from its middle, at most.
float reachOf(const BoxSpace::Solid& f)
{
  return f.circle ? f.half.x : glm::length(f.half);
}

// The cells of a raster that footprints come within `inflate` of (those
// whose heights reach low..high): CollisionSpace::raster()'s, and the broad
// phase's (every height).
void rasterOf(const std::vector<BoxSpace::Solid>& foot,
              float low,
              float high,
              glm::vec2 origin,
              float cell,
              int width,
              int height,
              float inflate,
              std::vector<uint8_t>& out)
{
  out.assign(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
  for (const BoxSpace::Solid& f : foot)
  {
    if (f.top < low || f.bottom > high)
      continue;
    const float reach = reachOf(f) + inflate;
    const glm::ivec2 lo =
        glm::ivec2(glm::floor((f.centre - reach - origin) / cell));
    const glm::ivec2 hi =
        glm::ivec2(glm::floor((f.centre + reach - origin) / cell));
    for (int y = std::max(lo.y, 0); y <= std::min(hi.y, height - 1); ++y)
      for (int x = std::max(lo.x, 0); x <= std::min(hi.x, width - 1); ++x)
      {
        const glm::vec2 p = origin + (glm::vec2(x, y) + 0.5f) * cell;
        float dist;
        if (!f.circle)
        {
          const glm::vec2 local = rotate(p - f.centre, -f.yaw);
          dist = glm::length(local - glm::clamp(local, -f.half, f.half));
        }
        else
          dist = glm::length(p - f.centre) - f.half.x;
        if (dist <= inflate)
          out[static_cast<std::size_t>(y) * width + x] = 1;
      }
  }
}
} // namespace

// One world's clearance: at least how far a point is from its nearest
// collider (any height), up to kReach (no probe reaches further). A world of
// a few colliders: them, exactly. Else a grid of lower bounds in tiles, each
// made the first time a query falls in it (a big world's plans visit a few:
// a new snapshot of the world costs only the tiles its plans visit): the
// distance field (distanceField()) of the cells the colliders touch over the
// tile and kReach round it, less the most a point and a collider's surface
// can be off their cells' middles.
class CollisionSpace::Clearance
{
public:
  explicit Clearance(std::vector<BoxSpace::Solid> foot)
  {
    if (foot.empty())
      return;
    m_all = {glm::vec2(std::numeric_limits<float>::max()),
             glm::vec2(std::numeric_limits<float>::lowest())};
    for (const BoxSpace::Solid& f : foot)
    {
      m_all.lo = glm::min(m_all.lo, f.centre - reachOf(f));
      m_all.hi = glm::max(m_all.hi, f.centre + reachOf(f));
    }
    if (foot.size() <= kFew)
    {
      for (const BoxSpace::Solid& f : foot)
        m_few.push_back({f, reachOf(f), unit(f.yaw)});
      return;
    }
    m_foot = std::move(foot);
    m_origin = m_all.lo - kReach;
    const float side = kCell * static_cast<float>(kTile);
    m_tw = std::max(
        1,
        static_cast<int>(std::ceil((m_all.hi.x + kReach - m_origin.x) / side)));
    m_th = std::max(
        1,
        static_cast<int>(std::ceil((m_all.hi.y + kReach - m_origin.y) / side)));
    const std::size_t tiles = static_cast<std::size_t>(m_tw) * m_th;
    m_tiles.resize(tiles);
    m_made = std::make_unique<std::atomic<bool>[]>(tiles);
    // (Which colliders each tile's field sees: those within kReach of it.)
    m_near.resize(tiles);
    for (uint32_t k = 0; k < m_foot.size(); ++k)
    {
      const BoxSpace::Solid& f = m_foot[k];
      const float r = reachOf(f) + kReach + kCell;
      const glm::ivec2 lo =
          glm::ivec2(glm::floor((f.centre - r - m_origin) / side));
      const glm::ivec2 hi =
          glm::ivec2(glm::floor((f.centre + r - m_origin) / side));
      for (int y = std::max(lo.y, 0); y <= std::min(hi.y, m_th - 1); ++y)
        for (int x = std::max(lo.x, 0); x <= std::min(hi.x, m_tw - 1); ++x)
          m_near[static_cast<std::size_t>(y) * m_tw + x].push_back(k);
    }
  }
  /** Whether every collider is further than `r` from `p`. */
  bool clear(glm::vec2 p, float r) const
  {
    if (m_all.lo.x > m_all.hi.x)
      return true;
    if (!m_few.empty())
    {
      // (A world of a few: each one's circle first (no trig), its exact
      // outline only when the circle's within reach.)
      for (const Few& f : m_few)
      {
        const glm::vec2 d = p - f.solid.centre;
        const float out = f.reach + r;
        if (glm::dot(d, d) > out * out)
          continue;
        float dist;
        if (f.solid.circle)
          dist = glm::length(d) - f.solid.half.x;
        else
        {
          const glm::vec2 local{d.x * f.axis.x + d.y * f.axis.y,
                                -d.x * f.axis.y + d.y * f.axis.x};
          dist = glm::length(local -
                             glm::clamp(local, -f.solid.half, f.solid.half));
        }
        if (dist <= r)
          return false;
      }
      return true;
    }
    return at(p) > r;
  }
  /** At least how far `p` is from every collider (m; 0 on or in one). */
  float at(glm::vec2 p) const
  {
    if (m_all.lo.x > m_all.hi.x)
      return std::numeric_limits<float>::infinity();
    if (!m_few.empty())
    {
      float least = std::numeric_limits<float>::infinity();
      for (const Few& f : m_few)
        least = std::min(least, distanceTo(f.solid, p));
      return std::max(least, 0.0f);
    }
    const float fx = (p.x - m_origin.x) / kCell,
                fy = (p.y - m_origin.y) / kCell;
    if (!(fx >= 0.0f && fy >= 0.0f))
      return glm::length(p - glm::clamp(p, m_all.lo, m_all.hi));
    const int cx = static_cast<int>(fx), cy = static_cast<int>(fy);
    const int tx = cx / kTile, ty = cy / kTile;
    // (Off the tiles: every collider's within the box round them all.)
    if (tx >= m_tw || ty >= m_th)
      return glm::length(p - glm::clamp(p, m_all.lo, m_all.hi));
    const std::size_t t = static_cast<std::size_t>(ty) * m_tw + tx;
    // (Made once, by whichever query comes first; never a lock once made:
    // std::call_once takes one global lock a call on some platforms.)
    if (!m_made[t].load(std::memory_order_acquire))
    {
      const std::lock_guard lock(m_making);
      if (!m_made[t].load(std::memory_order_relaxed))
      {
        make(tx, ty);
        m_made[t].store(true, std::memory_order_release);
      }
    }
    return m_tiles[t][static_cast<std::size_t>(cy - ty * kTile) * kTile +
                      static_cast<std::size_t>(cx - tx * kTile)];
  }

private:
  static constexpr std::size_t kFew = 48;
  static constexpr float kCell = 0.25f;
  static constexpr int kTile = 64;       // cells a tile's side
  static constexpr float kReach = 12.0f; // m: clearance told up to this
  // A tile's field: over it and kReach round it (a collider further off is
  // further than kReach from any point of it).
  void make(int tx, int ty) const
  {
    const int pad = static_cast<int>(std::ceil(kReach / kCell)) + 1;
    const int n = kTile + 2 * pad;
    const glm::vec2 lo =
        m_origin + (glm::vec2(static_cast<float>(tx), static_cast<float>(ty)) *
                        static_cast<float>(kTile) -
                    static_cast<float>(pad)) *
                       kCell;
    std::vector<BoxSpace::Solid> near;
    for (uint32_t k : m_near[static_cast<std::size_t>(ty) * m_tw + tx])
      near.push_back(m_foot[k]);
    std::vector<uint8_t> solid;
    // (Every cell any of them touches: a point of one lies within half a
    // cell's diagonal of its own cell's middle, so its cell is marked.)
    rasterOf(near,
             std::numeric_limits<float>::lowest(),
             std::numeric_limits<float>::max(),
             lo,
             kCell,
             n,
             n,
             0.71f * kCell,
             solid);
    const std::vector<float> field = distanceField(solid, n, n, kCell);
    // (A point in a cell and a collider's surface in a marked cell are each
    // within half a diagonal of their cells' middles; past kReach, kReach.)
    const float slack = 1.415f * kCell;
    std::vector<float>& tile =
        m_tiles[static_cast<std::size_t>(ty) * m_tw + tx];
    tile.resize(static_cast<std::size_t>(kTile) * kTile);
    for (int y = 0; y < kTile; ++y)
      for (int x = 0; x < kTile; ++x)
        tile[static_cast<std::size_t>(y) * kTile + x] = std::max(
            0.0f,
            std::min(field[static_cast<std::size_t>(y + pad) * n + x + pad],
                     kReach) -
                slack);
  }
  // (A world of a few's colliders, each with its reach and its yaw's axis.)
  struct Few
  {
    BoxSpace::Solid solid;
    float reach = 0.0f;
    glm::vec2 axis{1.0f, 0.0f};
  };
  std::vector<Few> m_few;
  std::vector<BoxSpace::Solid> m_foot;
  Rect m_all{glm::vec2(1.0f), glm::vec2(-1.0f)};
  glm::vec2 m_origin{0.0f};
  int m_tw = 0, m_th = 0;
  std::vector<std::vector<uint32_t>> m_near;
  mutable std::vector<std::vector<float>> m_tiles;
  std::unique_ptr<std::atomic<bool>[]> m_made;
  mutable std::mutex m_making;
};

namespace
{
// (Testing: every pose the broad phase settles asked of Jolt too, a miss
// logged as an error: SFS_BROAD_CHECK, development builds only.)
bool broadCheck()
{
  static const bool on =
      debugFeatures::enabled() && std::getenv("SFS_BROAD_CHECK") != nullptr;
  return on;
}
void broadMissed(Pose2 pose)
{
  static std::atomic<int> logged{0};
  if (logged++ >= 20)
    return;
  char line[200];
  std::snprintf(line,
                sizeof(line),
                "CollisionSpace: the broad phase called a pose clear that "
                "touches, at (%.3f, %.3f, %.4f)",
                pose.position.x,
                pose.position.y,
                pose.heading);
  LOG_ERROR(line);
}
// Whether a probe standing `at` is clear of a world by its clearance: each
// shape covered by circles along its length (a long box by a few, so one
// beside a fence isn't taken as near it for its length), each round its
// stretch of the shape as it stands, tilted (the reach over the ground of
// that stretch's half and the other two half axes: every point of the
// stretch is within it), clear of the world by more than a hair.
bool clearBy(const CollisionSpace::Clearance& clear,
             const Prepared& p,
             const Stance& at)
{
  for (const CollisionShape3D& s : p.shapes)
  {
    const glm::quat turn = at.rotation * s.rotation;
    const glm::mat3 axes = glm::mat3_cast(turn);
    const glm::vec3 half = glm::max(0.5f * s.size, glm::vec3(0.005f));
    const glm::vec2 along = glm::vec2(axes[0]) * half.x;
    const float side = half.y * glm::length(glm::vec2(axes[1])) +
                       half.z * glm::length(glm::vec2(axes[2]));
    const int n = std::clamp(
        static_cast<int>(std::ceil(half.x / std::max(half.y, 0.5f))), 1, 6);
    const float reach = glm::length(along) / static_cast<float>(n) + side;
    const glm::vec2 centre = glm::vec2(at.position + at.rotation * s.offset);
    for (int k = 0; k < n; ++k)
    {
      // (The k-th of n equal stretches along its length: its middle.)
      const float t =
          (2.0f * static_cast<float>(k) + 1.0f) / static_cast<float>(n) - 1.0f;
      if (!clear.clear(centre + along * t, reach + kHair))
        return false;
    }
  }
  return true;
}
} // namespace

Span spanOf(std::span<const Box3> boxes)
{
  Span s;
  bool any = false;
  for (const Box3& b : boxes)
  {
    s.back = any ? std::min(s.back, b.back) : b.back;
    s.front = any ? std::max(s.front, b.front) : b.front;
    s.half = std::max(s.half, std::abs(b.shift) + b.half);
    any = true;
  }
  return s;
}

Stance stanceOf(Pose2 pose,
                const Span& span,
                const std::function<float(glm::vec2)>& ground)
{
  const glm::quat yaw = yawRotation(pose.heading);
  if (!ground)
    return {{pose.position, 0.0f}, yaw};
  const glm::vec2 f(std::cos(pose.heading), std::sin(pose.heading));
  const glm::vec2 l(-f.y, f.x);
  const float z = ground(pose.position);
  const float length = span.front - span.back, width = 2.0f * span.half;
  if (length < 0.01f || width < 0.01f)
    return {{pose.position, z}, yaw};
  const glm::vec2 mid = pose.position + f * (0.5f * (span.back + span.front));
  const float zBack = ground(pose.position + f * span.back);
  const float zFront = ground(pose.position + f * span.front);
  const float zLeft = ground(mid + l * span.half);
  const float zRight = ground(mid - l * span.half);
  // (Nose up turns +x towards +z: about +y, negative. Left side up turns +y
  // towards +z: about +x, positive.)
  const float pitch = -std::atan2(zFront - zBack, length);
  const float roll = std::atan2(zLeft - zRight, width);
  return {{pose.position, z},
          yaw * glm::angleAxis(pitch, glm::vec3(0.0f, 1.0f, 0.0f)) *
              glm::angleAxis(roll, glm::vec3(1.0f, 0.0f, 0.0f))};
}

CollisionSpace::CollisionSpace(
    std::vector<std::shared_ptr<const CollisionWorld3D>> worlds,
    Options options)
    : m_worlds(std::move(worlds)), m_options(std::move(options))
{
  m_bounds = {glm::vec2(1e9f), glm::vec2(-1e9f)};
  for (const auto& w : m_worlds)
    for (const CollisionWorld3D::Body& b : w->bodies())
      for (const CollisionShape3D& s : b.shapes)
      {
        const float r =
            glm::length(glm::vec2(s.size)) + glm::length(glm::vec2(s.offset));
        const glm::vec2 c{b.position.x, b.position.y};
        m_bounds.lo = glm::min(m_bounds.lo, c - r);
        m_bounds.hi = glm::max(m_bounds.hi, c + r);
      }
  if (m_bounds.lo.x > m_bounds.hi.x)
    m_bounds = {glm::vec2(-1.0f), glm::vec2(1.0f)};
}

std::shared_ptr<const void> CollisionSpace::prepare(std::span<const Box3> boxes,
                                                    float grow,
                                                    float extend,
                                                    float widen) const
{
  std::vector<CollisionShape3D> shapes;
  for (const Box3& b : boxes)
  {
    CollisionShape3D s;
    s.kind = CollisionShape3D::Kind::Box;
    s.size = {b.front - b.back + 2.0f * (grow + extend),
              2.0f * (b.half + grow + widen),
              std::max(b.top - b.bottom, 0.01f)};
    s.offset = {0.5f * (b.back + b.front), b.shift, 0.5f * (b.bottom + b.top)};
    shapes.push_back(s);
  }
  return std::make_shared<const Prepared>(
      Prepared{CollisionWorld3D::Probe(shapes), spanOf(boxes), shapes});
}

const std::vector<std::shared_ptr<const CollisionSpace::Clearance>>&
CollisionSpace::clearances() const
{
  // (Every query asks: no lock once they're made.)
  if (m_clearMade.load(std::memory_order_acquire))
    return m_clear;
  std::call_once(
      m_clearOnce,
      [this]
      {
        if (!m_options.broadPhase)
        {
          m_clearMade.store(true, std::memory_order_release);
          return;
        }
        // (Kept by world while it lives, its layers and ignored ids: a static
        // world's grid is made once, however many spaces are made over it
        // (one per change of the parked machines).)
        struct Kept
        {
          std::weak_ptr<const CollisionWorld3D> world;
          const CollisionWorld3D* raw = nullptr;
          uint32_t layers = 0;
          std::vector<uint64_t> ignore;
          std::shared_ptr<const Clearance> clearance;
        };
        static std::mutex mutex;
        static std::vector<Kept> kept;
        const std::lock_guard lock(mutex);
        std::erase_if(kept, [](const Kept& k) { return k.world.expired(); });
        for (const auto& w : m_worlds)
        {
          std::shared_ptr<const Clearance> found;
          for (const Kept& k : kept)
            if (k.raw == w.get() && k.layers == m_options.layers &&
                k.ignore == m_options.ignore)
              found = k.clearance;
          if (!found)
          {
            found = std::make_shared<const Clearance>(
                nav::footprints(*w, m_options.layers, m_options.ignore));
            kept.push_back(
                {w, w.get(), m_options.layers, m_options.ignore, found});
          }
          m_clear.push_back(std::move(found));
        }
        m_clearMade.store(true, std::memory_order_release);
      });
  return m_clear;
}

bool CollisionSpace::fits(const void* prepared, Pose2 pose) const
{
  const Timed timed;
  const auto& p = *static_cast<const Prepared*>(prepared);
  const Stance at = stanceOf(pose, p.span, m_options.ground);
  const auto& clear = clearances();
  bool exact = false;
  for (std::size_t k = 0; k < m_worlds.size(); ++k)
  {
    if (k < clear.size() && clearBy(*clear[k], p, at))
    {
      if (broadCheck() && m_worlds[k]->overlaps(p.probe,
                                                at.position,
                                                at.rotation,
                                                m_options.layers,
                                                m_options.ignore))
        broadMissed(pose);
      continue;
    }
    exact = true;
    if (m_worlds[k]->overlaps(p.probe,
                              at.position,
                              at.rotation,
                              m_options.layers,
                              m_options.ignore))
    {
      ++checkStats().exact;
      return false;
    }
  }
  ++(exact ? checkStats().exact : checkStats().broad);
  return true;
}

std::optional<Space::Hit> CollisionSpace::hit(const void* prepared,
                                              Pose2 pose) const
{
  const Timed timed;
  const auto& p = *static_cast<const Prepared*>(prepared);
  const Stance at = stanceOf(pose, p.span, m_options.ground);
  const auto& clear = clearances();
  bool exact = false;
  for (std::size_t k = 0; k < m_worlds.size(); ++k)
  {
    if (k < clear.size() && clearBy(*clear[k], p, at))
    {
      if (broadCheck() && m_worlds[k]->overlaps(p.probe,
                                                at.position,
                                                at.rotation,
                                                m_options.layers,
                                                m_options.ignore))
        broadMissed(pose);
      continue;
    }
    exact = true;
    if (const auto h = m_worlds[k]->overlap(p.probe,
                                            at.position,
                                            at.rotation,
                                            m_options.layers,
                                            m_options.ignore))
    {
      ++checkStats().exact;
      return Hit{h->id, {h->point.x, h->point.y}};
    }
  }
  ++(exact ? checkStats().exact : checkStats().broad);
  return std::nullopt;
}

std::vector<BoxSpace::Solid>
footprints(const CollisionWorld3D& world,
           uint32_t layers,
           std::span<const uint64_t> ignore,
           const std::function<float(glm::vec2)>& ground)
{
  std::vector<BoxSpace::Solid> out;
  for (const CollisionWorld3D::Body& b : world.bodies())
  {
    if (!(layers & (1u << (b.layer & 31u))) ||
        std::find(ignore.begin(), ignore.end(), b.id) != ignore.end())
      continue;
    for (const CollisionShape3D& s : b.shapes)
    {
      const glm::quat turn = b.rotation * s.rotation;
      const glm::vec3 centre = b.position + b.rotation * s.offset;
      BoxSpace::Solid f;
      f.centre = {centre.x, centre.y};
      f.id = b.id;
      float up = 0.0f;
      if (s.kind == CollisionShape3D::Kind::Box)
      {
        const glm::vec3 h = 0.5f * s.size;
        const glm::vec3 ax = turn * glm::vec3(1.0f, 0.0f, 0.0f),
                        ay = turn * glm::vec3(0.0f, 1.0f, 0.0f),
                        az = turn * glm::vec3(0.0f, 0.0f, 1.0f);
        up = std::abs(ax.z) * h.x + std::abs(ay.z) * h.y + std::abs(az.z) * h.z;
        // (Its yaw: its own x turned onto the ground; standing upright x is
        // flat, tipped right up its y is.)
        const glm::vec2 flat = glm::length(glm::vec2(ax)) > 1.0e-3f
                                   ? glm::vec2(ax)
                                   : glm::vec2(ay);
        f.yaw = std::atan2(flat.y, flat.x);
        // (Its corners' reach along its yaw and across it: exact standing
        // upright, the upright box round it when tipped.)
        const glm::vec2 u(std::cos(f.yaw), std::sin(f.yaw)), v(-u.y, u.x);
        const auto reach = [&](glm::vec2 d)
        {
          return std::abs(glm::dot(glm::vec2(ax), d)) * h.x +
                 std::abs(glm::dot(glm::vec2(ay), d)) * h.y +
                 std::abs(glm::dot(glm::vec2(az), d)) * h.z;
        };
        f.half = {reach(u), reach(v)};
      }
      else
      {
        f.circle = true;
        f.half = glm::vec2(s.size.x);
        up = s.kind == CollisionShape3D::Kind::Capsule ? 0.5f * s.size.y
                                                       : s.size.x;
        // (A capsule lying over (its axis, its own Z, turned off upright):
        // its circle grown by its middle line's reach over the ground, as it
        // lies.)
        const glm::vec3 axis = turn * glm::vec3(0.0f, 0.0f, 1.0f);
        if (s.kind == CollisionShape3D::Kind::Capsule &&
            glm::length(glm::vec2(axis)) > 1.0e-4f)
        {
          const float half = std::max(0.5f * s.size.y - s.size.x, 0.005f);
          f.half = glm::vec2(s.size.x + glm::length(glm::vec2(axis)) * half);
          up = s.size.x + std::abs(axis.z) * half;
        }
      }
      const float under = ground ? ground(f.centre) : 0.0f;
      f.bottom = centre.z - up - under;
      f.top = centre.z + up - under;
      out.push_back(f);
    }
  }
  return out;
}

std::vector<BoxSpace::Solid> CollisionSpace::footprints() const
{
  std::call_once(
      m_footOnce,
      [this]
      {
        for (const auto& w : m_worlds)
        {
          std::vector<BoxSpace::Solid> each = nav::footprints(
              *w, m_options.layers, m_options.ignore, m_options.ground);
          m_foot.insert(m_foot.end(), each.begin(), each.end());
        }
      });
  return m_foot;
}

void CollisionSpace::raster(glm::vec2 origin,
                            float cell,
                            int width,
                            int height,
                            float inflate,
                            std::vector<uint8_t>& out) const
{
  footprints();
  rasterOf(m_foot,
           m_options.rasterLow,
           m_options.rasterHigh,
           origin,
           cell,
           width,
           height,
           inflate,
           out);
}

} // namespace sfs::nav
