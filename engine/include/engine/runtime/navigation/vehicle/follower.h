#pragma once

#include "engine/runtime/navigation/vehicle/vehiclePath.h"

#include <limits>

namespace sfs::nav::vehicle
{

/**
 * Following a path exactly (AI vehicles): the vehicle IS its path, its pose
 * the path's at the distance it's come, moved on at its profile's speed. The
 * caller may cap the speed (a traffic hold, the farm's working speed) and
 * name where to stop (`stopAt`, metres along): it brakes to stand there. Its
 * wheel's angle and a trailer's angle are the path's there, for looks.
 */
class Follower
{
public:
  /**
   * How it drives (its spec's, derived): the time a direction change stands
   * it (its shuttle: the wheel goes on turning as it moves off), and its
   * acceleration and braking (m/s2).
   */
  struct Drive
  {
    float shuttle = 0.5f, accel = 1.2f, brake = 2.0f;
  };
  void setDrive(const Drive& drive) { m_drive = drive; }
  const Drive& drive() const { return m_drive; }
  void setPath(VehiclePath path, float speed = 0.0f);
  const VehiclePath& path() const { return m_path; }

  struct State
  {
    bool done = false;    // at the end
    bool waiting = false; // standing at a cusp, changing gear
  };
  State step(float deltaTime,
             float speedCap = std::numeric_limits<float>::infinity(),
             float stopAt = std::numeric_limits<float>::infinity());

  float s() const { return m_s; }
  /** Signed: below 0 backing up. */
  float speed() const { return m_speed * static_cast<float>(here().gear); }
  Pose2 pose() const;
  float curvature() const;
  float hitch() const;
  /** The bicycle's steering angle for the curvature (radians, + left). */
  float steerAngle(float wheelbase) const;
  /** The index of the point at or before s. */
  std::size_t index() const { return m_index; }

private:
  const PathPoint& here() const;
  PathPoint at(float s) const;
  VehiclePath m_path;
  float m_s = 0.0f, m_speed = 0.0f, m_dwell = 0.0f;
  Drive m_drive;
  std::size_t m_index = 0;
  int m_gear = 1;
};

/**
 * The bicycle model, for a player's driving (not AI): a rear axle rolling
 * straight, the front wheel steered.
 */
struct Bicycle
{
  static Pose2 step(Pose2 rear, float distance, float steer, float wheelbase);
  static float radius(float steer, float wheelbase);
};

} // namespace sfs::nav::vehicle
