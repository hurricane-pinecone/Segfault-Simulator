#include "engine/runtime/navigation/vehicle/follower.h"

#include "engine/runtime/navigation/fleet.h"
#include "engine/runtime/navigation/vehicle/steering.h"

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
  const float brake = std::max(m_drive.brake, kLeastRate);
  const float accel = std::max(m_drive.accel, kLeastRate);
  // (The fastest it may go this step and still stand at the stop braking at
  // its rate: stoppingDistance(v, brake, dt) = left, so the step itself
  // counts and it never falls behind its brakes.)
  const float bt = brake * dt;
  const float stopSpeed = Follower::stopSpeed(left, brake, dt);
  target = std::min(target, stopSpeed);
  if (left > 0.01f && speedCap > 0.0f)
    target = std::max(target, std::min(kCreep, speedCap));
  // (A stop already nearer than its brakes reach from the speed it's going:
  // one it can't brake for.)
  const bool late = m_speed > stopSpeed + bt + kCreep;
  const float was = m_speed;
  // (At its derived rate, the one its speed profile was priced with.)
  m_speed = target < m_speed ? std::max(target, m_speed - brake * dt)
                             : std::min(target, m_speed + accel * dt);
  m_s = std::min(m_s + m_speed * dt, standAt);
  if (standAt - m_s < 1.0e-3f)
  {
    // (A stop named nearer than its brakes reach: stood all the same, never
    // past it, and told (State::hardStopFrom).)
    if (late)
      st.hardStopFrom = was;
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

float Follower::stoppingDistance(float speed, float brake, float dt)
{
  const float v = std::abs(speed);
  return v * std::max(dt, 0.0f) + v * v / (2.0f * std::max(brake, kLeastRate));
}

float Follower::stopSpeed(float distance, float brake, float dt)
{
  const float b = std::max(brake, kLeastRate);
  const float bt = b * std::max(dt, 0.0f);
  return -bt + std::sqrt(bt * bt + 2.0f * b * std::max(distance, 0.0f));
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

void HandDrive::start(Pose2 pose, float hitch, float speed)
{
  m_pose = pose;
  m_hitch = hitch;
  m_speed = speed;
  m_steer = 0.0f;
  m_dwell = 0.0f;
  m_gear = speed < 0.0f ? -1 : 1;
}

float HandDrive::fullLock() const
{
  return std::atan(std::max(m_model.wheelbase, 0.05f) /
                   std::max(m_model.minRadius, 0.01f));
}

float HandDrive::curvature() const
{
  return std::tan(m_steer) / std::max(m_model.wheelbase, 0.05f);
}

void HandDrive::step(const Controls& controls, float dt, float speedCap)
{
  if (dt <= 0.0f)
    return;
  // The wheel turns toward what's asked at its steer rate, never past lock.
  const float lock = fullLock();
  const float want = std::clamp(controls.steer, -1.0f, 1.0f) * lock;
  const float turn = std::max(m_model.steerRate, 0.0f) * dt;
  m_steer = std::clamp(
      m_steer + std::clamp(want - m_steer, -turn, turn), -lock, lock);
  const int gear = controls.gear < 0 && m_model.mayReverse ? -1 : 1;
  const float throttle = std::clamp(controls.throttle, 0.0f, 1.0f);
  const float braking = std::clamp(controls.brake, 0.0f, 1.0f);
  const float accel = std::max(m_model.accel, Follower::kLeastRate);
  const float brake = std::max(m_model.brake, Follower::kLeastRate);
  float v = std::abs(m_speed);
  if (m_dwell > 0.0f)
  {
    // Standing to change gear (its shuttle).
    m_dwell -= dt;
    m_speed = 0.0f;
    return;
  }
  if (gear != m_gear)
  {
    // The other gear asked for: to a stand first (the drive holding it back,
    // and the brake), then the change of direction takes its time.
    v = std::max(v - (accel + brake * braking) * dt, 0.0f);
    if (v < kStill)
    {
      v = 0.0f;
      m_gear = gear;
      m_dwell = std::max(m_model.shuttleSeconds, 0.0f);
    }
  }
  else
  {
    // Its share of the gear's top speed, no faster than the curve allows
    // (the speed profile's own limit) or the caller's cap.
    const float top = m_model.speedAt(gear, curvature(), 0.0f);
    const float target = std::min(throttle * top, std::max(speedCap, 0.0f));
    v = v < target ? std::min(target, v + accel * dt)
                   : std::max(target, v - accel * dt);
    v = std::max(v - brake * braking * dt, 0.0f);
  }
  const float was = m_speed;
  m_speed = static_cast<float>(m_gear) * v;
  const float ds = 0.5f * (was + m_speed) * dt;
  if (std::abs(ds) < 1.0e-7f)
    return;
  const float k = curvature();
  m_pose = Bicycle::step(m_pose, ds, m_steer, m_model.wheelbase);
  if (m_model.trailer)
  {
    const Trailer& t = *m_model.trailer;
    const float limit = ds < 0.0f ? t.reverseLimit : t.forwardLimit;
    m_hitch = std::clamp(hitchStep(t, m_hitch, ds, k), -limit, limit);
  }
}

} // namespace sfs::nav::vehicle
