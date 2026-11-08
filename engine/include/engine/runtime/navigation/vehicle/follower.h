#pragma once

#include "engine/runtime/navigation/agentKind.h"
#include "engine/runtime/navigation/vehicle/vehicleModel.h"
#include "engine/runtime/navigation/vehicle/vehiclePath.h"

#include <limits>

namespace sfs::nav::vehicle
{

/**
 * Following a path exactly (AI vehicles): the vehicle IS its path, its pose
 * the path's at the distance it's come, moved on at its profile's speed. The
 * caller may cap the speed (a traffic hold, the farm's working speed) and
 * name where to stop (`stopAt`, metres along): it brakes to stand there (at
 * its derived rate, the one its speed profile is priced with; never past a
 * stop named too late: stood there, told as a hard stop). Its
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
    // Stood at `stopAt` (or a stop) from this speed (m/s), faster than its
    // brakes could take off: a stop named too late, never priced. 0: none.
    float hardStopFrom = 0.0f;
  };
  /** m/s it creeps at, at least, while there's way left to a stop. */
  static constexpr float kCreep = 0.15f;
  /** m/s2: the least braking or pulling away it's reckoned with. */
  static constexpr float kLeastRate = 0.05f;
  /**
   * THE stopping distance (m): from `speed` braking at `brake` (its spec's,
   * derived), the step `dt` it goes on at that speed first counted (v * dt +
   * v^2 / 2b). The follower stands by it; anyone reckoning how far a vehicle
   * goes before it can stand (a look-ahead, a hand-over) calls this.
   */
  static float stoppingDistance(float speed, float brake, float dt = 0.0f);
  /** Its inverse: the fastest it may go and still stand within `distance`. */
  static float stopSpeed(float distance, float brake, float dt = 0.0f);
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

/**
 * A vehicle driven by hand (a player at the wheel): the controls move it by
 * the same model its paths are driven by (VehicleModel: one physics source).
 * The wheel turns toward the steering asked for at steerRate, never past
 * full lock (minRadius); the throttle asks for its share of the gear's top
 * speed, no faster than VehicleModel::speedAt() allows on the curve it's on
 * (the speed profile's function), reached at the derived accel, and the
 * drive holds it back to that at the same rate when it's let off (the
 * drivetrain carries the same force either way); the brake adds its derived
 * brake rate. Asked for the other gear while moving, it brakes to a stand
 * first, then stands shuttleSeconds before it moves off the other way (as
 * the follower does at a cusp). A trailer swings on its hitch
 * (hitchStep()) within its limits. Its pose is its reference point's (the
 * fixed axle), moved as the bicycle model moves it.
 */
class HandDrive
{
public:
  void setModel(const VehicleModel& model) { m_model = model; }
  const VehicleModel& model() const { return m_model; }
  /** From here, at `speed` (signed) with the wheel straight. */
  void start(Pose2 pose, float hitch, float speed);
  /** On by `dt` with `controls`, no faster than `speedCap` (m/s). */
  void step(const Controls& controls,
            float dt,
            float speedCap = std::numeric_limits<float>::infinity());
  Pose2 pose() const { return m_pose; }
  float hitch() const { return m_hitch; }
  /** Signed: below 0 backing up. */
  float speed() const { return m_speed; }
  /** The wheel's angle (radians, + left) and the curvature it turns. */
  float steerAngle() const { return m_steer; }
  float curvature() const;
  /** Its fullest lock (radians), from wheelbase and minRadius. */
  float fullLock() const;
  /** Standing to change gear (s left). */
  float shuttleLeft() const { return m_dwell; }

private:
  VehicleModel m_model;
  Pose2 m_pose;
  float m_hitch = 0.0f, m_speed = 0.0f, m_steer = 0.0f, m_dwell = 0.0f;
  int m_gear = 1;
};

} // namespace sfs::nav::vehicle
