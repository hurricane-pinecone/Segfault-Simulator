#include "engine/runtime/navigation/vehicle/follower.h"

#include <algorithm>
#include <cmath>

namespace sfs::nav::vehicle
{

void Follower::setPath(VehiclePath path, float speed)
{
  m_path = std::move(path);
  m_s = 0.0f;
  m_speed = std::abs(speed);
  m_dwell = 0.0f;
  m_index = 0;
  m_gear = m_path.points.empty() ? 1 : m_path.points.front().gear;
}

const PathPoint& Follower::here() const
{
  static const PathPoint none;
  return m_path.points.empty() ? none : m_path.points[m_index];
}

PathPoint Follower::at(float s) const
{
  const std::vector<PathPoint>& p = m_path.points;
  if (p.empty())
    return {};
  std::size_t i = m_index;
  while (i + 1 < p.size() && p[i + 1].s <= s)
    ++i;
  if (i + 1 >= p.size())
    return p.back();
  const PathPoint& a = p[i];
  const PathPoint& b = p[i + 1];
  const float t =
      b.s > a.s ? std::clamp((s - a.s) / (b.s - a.s), 0.0f, 1.0f) : 0.0f;
  PathPoint out = a;
  out.s = s;
  out.pose.position = a.pose.position + t * (b.pose.position - a.pose.position);
  out.pose.heading =
      a.pose.heading + t * wrapAngle(b.pose.heading - a.pose.heading);
  out.curvature = a.curvature + t * (b.curvature - a.curvature);
  out.hitch = a.hitch + t * wrapAngle(b.hitch - a.hitch);
  out.speed = a.speed + t * (b.speed - a.speed);
  return out;
}

Follower::State Follower::step(float dt, float speedCap, float stopAt)
{
  State st;
  const std::vector<PathPoint>& p = m_path.points;
  if (p.size() < 2)
  {
    st.done = true;
    return st;
  }
  // Standing at a cusp: the shuttle change takes its time.
  if (m_dwell > 0.0f)
  {
    m_dwell -= dt;
    st.waiting = true;
    return st;
  }
  // The next place it must stand: a cusp ahead, the end, or `stopAt`.
  std::size_t i = m_index;
  while (i + 1 < p.size() && p[i + 1].gear == p[m_index].gear)
    ++i;
  const float standAt = std::min(p[i].s, stopAt);
  const PathPoint now = at(m_s);
  float target = std::min(now.speed, speedCap);
  // (Never below a creep while there's way to go: the profile stands only
  // at its stops.)
  const float left = standAt - m_s;
  // (Its own braking and pulling away: derived from its spec and load.)
  const float brake = std::max(m_drive.brake, 0.05f);
  const float accel = std::max(m_drive.accel, 0.05f);
  target = std::min(target, std::sqrt(2.0f * brake * std::max(left, 0.0f)));
  if (left > 0.01f && speedCap > 0.0f)
    target = std::max(target, std::min(0.15f, speedCap));
  m_speed = target < m_speed ? std::max(target, m_speed - brake * 2.0f * dt)
                             : std::min(target, m_speed + accel * dt);
  m_s = std::min(m_s + m_speed * dt, standAt);
  if (standAt - m_s < 1.0e-3f)
  {
    m_s = standAt; // (there: no creeping the last millimetre for ever)
    m_speed = 0.0f;
  }
  while (m_index + 1 < p.size() && p[m_index + 1].s <= m_s &&
         p[m_index + 1].gear == p[m_index].gear)
    ++m_index;
  // At a cusp: change direction (standing a while), on along the next stretch.
  if (m_s >= p[i].s && i + 1 < p.size() && p[i].s <= stopAt)
  {
    m_index = i + 1; // (the cusp's second point: the same place, the new gear)
    m_dwell = std::max(m_drive.shuttle, 0.0f);
    m_speed = 0.0f;
  }
  st.done = m_s >= p.back().s - 1.0e-4f;
  return st;
}

Pose2 Follower::pose() const { return at(m_s).pose; }
float Follower::curvature() const { return at(m_s).curvature; }
float Follower::hitch() const { return at(m_s).hitch; }
float Follower::steerAngle(float wheelbase) const
{
  return std::atan(curvature() * wheelbase);
}

Pose2 Bicycle::step(Pose2 rear, float distance, float steer, float wheelbase)
{
  const float k = std::tan(steer) / wheelbase;
  if (std::abs(k) < 1.0e-7f)
    return {rear.position + distance * unit(rear.heading), rear.heading};
  const float h = rear.heading + k * distance;
  return {rear.position + glm::vec2(std::sin(h) - std::sin(rear.heading),
                                    std::cos(rear.heading) - std::cos(h)) /
                              k,
          h};
}

float Bicycle::radius(float steer, float wheelbase)
{
  return std::abs(steer) < 1.0e-6f ? std::numeric_limits<float>::infinity()
                                   : wheelbase / std::tan(std::abs(steer));
}

} // namespace sfs::nav::vehicle
