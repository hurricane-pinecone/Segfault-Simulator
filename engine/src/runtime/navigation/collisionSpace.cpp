#include "engine/runtime/navigation/collisionSpace.h"

#include <algorithm>
#include <cmath>

namespace sfs::nav
{

namespace
{
// A body's turn about Z (bodies on the ground stand upright).
float yawOf(const glm::quat& q)
{
  return std::atan2(
      2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
}
glm::vec2 rotate(glm::vec2 v, float a)
{
  const float c = std::cos(a), s = std::sin(a);
  return {c * v.x - s * v.y, s * v.x + c * v.y};
}
} // namespace

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

glm::vec3 CollisionSpace::at(Pose2 pose) const
{
  const float z = m_options.ground ? m_options.ground(pose.position) : 0.0f;
  return {pose.position, z};
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
  return std::make_shared<const CollisionWorld3D::Probe>(shapes);
}

bool CollisionSpace::fits(const void* prepared, Pose2 pose) const
{
  const auto& probe = *static_cast<const CollisionWorld3D::Probe*>(prepared);
  const glm::vec3 p = at(pose);
  const glm::quat r = yawRotation(pose.heading);
  for (const auto& w : m_worlds)
    if (w->overlaps(probe, p, r, m_options.layers, m_options.ignore))
      return false;
  return true;
}

std::optional<Space::Hit> CollisionSpace::hit(const void* prepared,
                                              Pose2 pose) const
{
  const auto& probe = *static_cast<const CollisionWorld3D::Probe*>(prepared);
  const glm::vec3 p = at(pose);
  const glm::quat r = yawRotation(pose.heading);
  for (const auto& w : m_worlds)
    if (const auto h =
            w->overlap(probe, p, r, m_options.layers, m_options.ignore))
      return Hit{h->id, {h->point.x, h->point.y}};
  return std::nullopt;
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
  for (const auto& w : m_worlds)
    for (const CollisionWorld3D::Body& b : w->bodies())
    {
      if (!(m_options.layers & (1u << (b.layer & 31u))) ||
          std::find(m_options.ignore.begin(), m_options.ignore.end(), b.id) !=
              m_options.ignore.end())
        continue;
      const float yaw = yawOf(b.rotation);
      for (const CollisionShape3D& s : b.shapes)
      {
        const glm::vec3 off = b.rotation * s.offset;
        const float z = b.position.z + off.z;
        const float halfH =
            s.kind == CollisionShape3D::Kind::Box       ? 0.5f * s.size.z
            : s.kind == CollisionShape3D::Kind::Capsule ? 0.5f * s.size.y
                                                        : s.size.x;
        const float ground =
            m_options.ground ? m_options.ground({b.position.x, b.position.y})
                             : 0.0f;
        if (z + halfH - ground < m_options.rasterLow ||
            z - halfH - ground > m_options.rasterHigh)
          continue;
        const glm::vec2 c{b.position.x + off.x, b.position.y + off.y};
        const bool box = s.kind == CollisionShape3D::Kind::Box;
        const glm::vec2 half = box ? glm::vec2(0.5f * s.size.x, 0.5f * s.size.y)
                                   : glm::vec2(s.size.x);
        const float a = yaw + (box ? yawOf(s.rotation) : 0.0f);
        const float reach = (box ? glm::length(half) : half.x) + inflate;
        const glm::ivec2 lo =
            glm::ivec2(glm::floor((c - reach - origin) / cell));
        const glm::ivec2 hi =
            glm::ivec2(glm::floor((c + reach - origin) / cell));
        for (int y = std::max(lo.y, 0); y <= std::min(hi.y, height - 1); ++y)
          for (int x = std::max(lo.x, 0); x <= std::min(hi.x, width - 1); ++x)
          {
            const glm::vec2 p = origin + (glm::vec2(x, y) + 0.5f) * cell;
            float dist;
            if (box)
            {
              const glm::vec2 local = rotate(p - c, -a);
              dist = glm::length(local - glm::clamp(local, -half, half));
            }
            else
              dist = glm::length(p - c) - half.x;
            if (dist <= inflate)
              out[static_cast<std::size_t>(y) * width + x] = 1;
          }
      }
    }
}

} // namespace sfs::nav
