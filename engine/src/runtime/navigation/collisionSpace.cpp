#include "engine/runtime/navigation/collisionSpace.h"

#include <algorithm>
#include <cmath>

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
// (What prepare() hands back: the probe and the span it stands on.)
struct Prepared
{
  CollisionWorld3D::Probe probe;
  Span span;
};
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
      Prepared{CollisionWorld3D::Probe(shapes), spanOf(boxes)});
}

bool CollisionSpace::fits(const void* prepared, Pose2 pose) const
{
  const auto& p = *static_cast<const Prepared*>(prepared);
  const Stance at = stanceOf(pose, p.span, m_options.ground);
  for (const auto& w : m_worlds)
    if (w->overlaps(p.probe,
                    at.position,
                    at.rotation,
                    m_options.layers,
                    m_options.ignore))
      return false;
  return true;
}

std::optional<Space::Hit> CollisionSpace::hit(const void* prepared,
                                              Pose2 pose) const
{
  const auto& p = *static_cast<const Prepared*>(prepared);
  const Stance at = stanceOf(pose, p.span, m_options.ground);
  for (const auto& w : m_worlds)
    if (const auto h = w->overlap(p.probe,
                                  at.position,
                                  at.rotation,
                                  m_options.layers,
                                  m_options.ignore))
      return Hit{h->id, {h->point.x, h->point.y}};
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
  std::vector<BoxSpace::Solid> out;
  for (const auto& w : m_worlds)
  {
    std::vector<BoxSpace::Solid> each = nav::footprints(
        *w, m_options.layers, m_options.ignore, m_options.ground);
    out.insert(out.end(), each.begin(), each.end());
  }
  return out;
}

void CollisionSpace::raster(glm::vec2 origin,
                            float cell,
                            int width,
                            int height,
                            float inflate,
                            std::vector<uint8_t>& out) const
{
  out.assign(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
  for (const BoxSpace::Solid& f : footprints())
  {
    if (f.top < m_options.rasterLow || f.bottom > m_options.rasterHigh)
      continue;
    const float reach = (f.circle ? f.half.x : glm::length(f.half)) + inflate;
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

} // namespace sfs::nav
