#include "engine/runtime/navigation/space.h"
#include "engine/runtime/navigation/costLayer.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sfs::nav
{

CheckStats& checkStats()
{
  thread_local CheckStats stats;
  return stats;
}

void CostLayer::raster(glm::vec2,
                       float,
                       int width,
                       int height,
                       int,
                       std::vector<float>& out) const
{
  out.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
             floor());
}

namespace
{
struct Obb
{
  glm::vec2 c, half;
  float yaw;
  float bottom, top;
};
glm::vec2 leftOf(glm::vec2 f) { return {-f.y, f.x}; }
bool overlap(const Obb& a, const Obb& b)
{
  if (a.top <= b.bottom || b.top <= a.bottom)
    return false;
  const glm::vec2 fa = unit(a.yaw), fb = unit(b.yaw);
  const std::array<glm::vec2, 4> axes{fa, leftOf(fa), fb, leftOf(fb)};
  const glm::vec2 d = b.c - a.c;
  for (const glm::vec2& ax : axes)
  {
    const float ra = a.half.x * std::abs(glm::dot(fa, ax)) +
                     a.half.y * std::abs(glm::dot(leftOf(fa), ax));
    const float rb = b.half.x * std::abs(glm::dot(fb, ax)) +
                     b.half.y * std::abs(glm::dot(leftOf(fb), ax));
    if (std::abs(glm::dot(d, ax)) > ra + rb)
      return false;
  }
  return true;
}
// A box and a circle: the circle's middle within `r` of the box.
bool overlapCircle(const Obb& a, glm::vec2 c, float r, float bottom, float top)
{
  if (a.top <= bottom || top <= a.bottom)
    return false;
  const glm::vec2 f = unit(a.yaw), l = leftOf(f), d = c - a.c;
  const glm::vec2 local{glm::dot(d, f), glm::dot(d, l)};
  const glm::vec2 q = glm::clamp(local, -a.half, a.half);
  return glm::length(local - q) < r;
}
struct Prepared
{
  std::vector<Box3> boxes;
};
Obb place(const Box3& b, Pose2 pose)
{
  const glm::vec2 f = unit(pose.heading);
  const glm::vec2 mid =
      pose.position + f * (0.5f * (b.back + b.front)) + leftOf(f) * b.shift;
  return {
      mid, {0.5f * (b.front - b.back), b.half}, pose.heading, b.bottom, b.top};
}
} // namespace

BoxSpace::BoxSpace(std::vector<Solid> solids) : m_solids(std::move(solids))
{
  m_bounds = {glm::vec2(1e9f), glm::vec2(-1e9f)};
  for (const Solid& s : m_solids)
  {
    const float r = glm::length(s.half);
    m_bounds.lo = glm::min(m_bounds.lo, s.centre - r);
    m_bounds.hi = glm::max(m_bounds.hi, s.centre + r);
  }
  if (m_solids.empty())
    m_bounds = {glm::vec2(-1.0f), glm::vec2(1.0f)};
}

std::shared_ptr<const void> BoxSpace::prepare(std::span<const Box3> boxes,
                                              float grow,
                                              float extend,
                                              float widen) const
{
  auto p = std::make_shared<Prepared>();
  for (Box3 b : boxes)
  {
    b.back -= grow + extend;
    b.front += grow + extend;
    b.half += grow + widen;
    p->boxes.push_back(b);
  }
  return p;
}

std::optional<Space::Hit> BoxSpace::hit(const void* prepared, Pose2 pose) const
{
  const auto* p = static_cast<const Prepared*>(prepared);
  for (const Box3& b : p->boxes)
  {
    const Obb a = place(b, pose);
    for (const Solid& s : m_solids)
    {
      const bool touch =
          s.circle ? overlapCircle(a, s.centre, s.half.x, s.bottom, s.top)
                   : overlap(a, {s.centre, s.half, s.yaw, s.bottom, s.top});
      if (touch)
        return Hit{s.id, s.centre};
    }
  }
  return std::nullopt;
}

bool BoxSpace::fits(const void* prepared, Pose2 pose) const
{
  return !hit(prepared, pose);
}

void BoxSpace::raster(glm::vec2 origin,
                      float cell,
                      int width,
                      int height,
                      float inflate,
                      std::vector<uint8_t>& out) const
{
  out.assign(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
  for (const Solid& s : m_solids)
  {
    const float reach = (s.circle ? s.half.x : glm::length(s.half)) + inflate;
    const glm::ivec2 lo =
        glm::ivec2(glm::floor((s.centre - reach - origin) / cell));
    const glm::ivec2 hi =
        glm::ivec2(glm::floor((s.centre + reach - origin) / cell));
    for (int y = std::max(lo.y, 0); y <= std::min(hi.y, height - 1); ++y)
      for (int x = std::max(lo.x, 0); x <= std::min(hi.x, width - 1); ++x)
      {
        const glm::vec2 c = origin + (glm::vec2(x, y) + 0.5f) * cell;
        float dist;
        if (s.circle)
          dist = glm::length(c - s.centre) - s.half.x;
        else
        {
          const glm::vec2 f = unit(s.yaw), d = c - s.centre;
          const glm::vec2 local{glm::dot(d, f), glm::dot(d, leftOf(f))};
          dist = glm::length(local - glm::clamp(local, -s.half, s.half));
        }
        if (dist <= inflate)
          out[static_cast<std::size_t>(y) * width + x] = 1;
      }
  }
}

namespace
{
struct BothPrepared
{
  std::shared_ptr<const void> base, solids;
};
} // namespace

SpaceWithSolids::SpaceWithSolids(std::shared_ptr<const Space> base,
                                 std::vector<BoxSpace::Solid> solids)
    : m_base(std::move(base)), m_none(solids.empty()),
      m_solids(std::move(solids))
{
}

std::shared_ptr<const void>
SpaceWithSolids::prepare(std::span<const Box3> boxes,
                         float grow,
                         float extend,
                         float widen) const
{
  auto p = std::make_shared<BothPrepared>();
  p->base = m_base->prepare(boxes, grow, extend, widen);
  p->solids = m_solids.prepare(boxes, grow, extend, widen);
  return p;
}

bool SpaceWithSolids::fits(const void* prepared, Pose2 pose) const
{
  const auto* p = static_cast<const BothPrepared*>(prepared);
  return m_solids.fits(p->solids.get(), pose) &&
         m_base->fits(p->base.get(), pose);
}

std::optional<Space::Hit> SpaceWithSolids::hit(const void* prepared,
                                               Pose2 pose) const
{
  const auto* p = static_cast<const BothPrepared*>(prepared);
  if (auto h = m_solids.hit(p->solids.get(), pose))
    return h;
  return m_base->hit(p->base.get(), pose);
}

void SpaceWithSolids::raster(glm::vec2 origin,
                             float cell,
                             int width,
                             int height,
                             float inflate,
                             std::vector<uint8_t>& out) const
{
  m_base->raster(origin, cell, width, height, inflate, out);
  std::vector<uint8_t> more;
  m_solids.raster(origin, cell, width, height, inflate, more);
  for (std::size_t i = 0; i < out.size() && i < more.size(); ++i)
    out[i] |= more[i];
}

Rect SpaceWithSolids::bounds() const
{
  Rect a = m_base->bounds();
  if (m_none)
    return a;
  const Rect b = m_solids.bounds();
  return {glm::min(a.lo, b.lo), glm::max(a.hi, b.hi)};
}

float CombinedCost::factor(Pose2 pose, int mode) const
{
  float f = 1.0f;
  for (const auto& l : m_layers)
    f *= l->factor(pose, mode);
  return f;
}

float CombinedCost::floor() const
{
  float f = 1.0f;
  for (const auto& l : m_layers)
    f *= l->floor();
  return f;
}

void CombinedCost::raster(glm::vec2 origin,
                          float cell,
                          int width,
                          int height,
                          int mode,
                          std::vector<float>& out) const
{
  out.assign(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 1.0f);
  std::vector<float> one;
  for (const auto& l : m_layers)
  {
    l->raster(origin, cell, width, height, mode, one);
    for (std::size_t i = 0; i < out.size() && i < one.size(); ++i)
      out[i] *= one[i];
  }
}

bool CombinedCost::noWait(glm::vec2 p) const
{
  for (const auto& l : m_layers)
    if (l->noWait(p))
      return true;
  return false;
}

} // namespace sfs::nav
