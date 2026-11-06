#pragma once

#include "engine/runtime/navigation/geometry.h"

#include <algorithm>
#include <optional>
#include <vector>

/**
 * sfs::nav::vehicle: the "vehicle" agent kind (cars, tractors, rigs towing a
 * trailer). The textbook stack: continuous-curvature steering (HC/CC
 * Reeds-Shepp), a path search over those motions (hybrid A* built in), a
 * smoother, a speed profile and a follower that drives the path exactly. A
 * vehicle's pose is that of its reference point: the middle of its fixed
 * (rear) axle, the point that turns on a circle.
 */
namespace sfs::nav::vehicle
{

/**
 * A trailer on a hitch `hitchBehind` behind the reference point, its own axle
 * `drawbar` behind the hitch; its box is in the frame of its axle. Its angle
 * on the hitch (+: the tractor turned left of it) never passes forwardLimit
 * going forward nor reverseLimit backing up (backing, it stays tight so the
 * rig never jack-knifes).
 */
struct Trailer
{
  float hitchBehind = 1.0f, drawbar = 4.0f;
  Box3 box;
  float forwardLimit = 1.45f; // ~83 degrees
  float reverseLimit = 0.61f; // ~35 degrees
};

/**
 * The costs a plan is priced by, all in SECONDS (the time the motion takes,
 * so a three-point turn and a loop are compared by how long each takes), in
 * one place. The defaults are the farm-helper ones; a game sets its own.
 */
struct Tunables
{
  float reversePenalty = 1.5f; // a second backing up costs this many
  float turnSpeed = 2.0f;      // m/s through the steering transitions
  float searchWeight = 1.5f;   // weighted A*: heuristic x this
  float cell = 0.5f;           // m, the search's position bins
  float headingBin = 0.0873f;  // rad (5 degrees)
  float openClear = 1.5f;      // m clear past its reach: coarser bins there
  float hitchBin = 0.35f;      // rad, a trailer's angle bins
  float checkStep = 0.3f;      // m between poses the search checks
  float exactStep = 0.1f;      // m between poses of the final check
  float gap = 0.02f;           // m round the exact boxes: THE gap
  float heuristicCell = 0.5f;  // m, the 2D heuristic's raster
  float goalShotEvery = 6.0f;  // m: an analytic shot about this often
};

/**
 * A vehicle as the planner sees it. Its curvature changes no faster than its
 * wheel turns: `steerRate` (rad/s at the wheel) over the wheelbase is how
 * fast curvature may change a second; the path's steering transitions are
 * laid for Tunables::turnSpeed, and the speed profile slows wherever the
 * curvature changes faster than that allows.
 */
struct VehicleModel
{
  std::vector<Box3> boxes; // the exact collider boxes (not the trailer's)
  float minRadius = 5.0f;  // at the reference point, wheel on full lock
  float wheelbase = 2.6f;  // m, reference point to the steered axle
  float steerRate = 1.2f;  // rad/s at the wheel
  float topSpeed = 7.0f, reverseSpeed = 2.0f; // m/s
  float accel = 1.2f, brake = 2.0f;           // m/s^2
  float lateralAccel = 0.9f;                  // m/s^2 round a curve
  float shuttleSeconds = 0.5f; // s it stands at a change of direction
  bool mayReverse = true;
  std::optional<Trailer> trailer;
  /**
   * A working mode (an implement down): its own boxes and tightest turn,
   * forward only if it must be. Mode 0 is plain travel (the fields above);
   * the CostLayer is told which mode a pose is driven in.
   */
  struct Mode
  {
    std::vector<Box3> boxes;
    float minRadius = 5.0f;
    bool forwardOnly = true;
  };
  std::vector<Mode> modes; // modes 1.. (index + 1)

  /** The largest curvature (1/m). */
  float maxCurvature() const { return 1.0f / std::max(minRadius, 0.01f); }
  /** How fast curvature may change a second (1/m/s). */
  float curvatureRate() const { return steerRate / wheelbase; }
  /**
   * The time a change of direction (a cusp) really costs, as the follower
   * drives it: standing shuttleSeconds, plus the time lost braking to a stand
   * from the speed it goes there and pulling away again at its own brake and
   * accel (v/2b + v/2a: the time over driving those metres at speed). The
   * speed there: turnSpeed (Tunables) or slower, one side forward and the
   * other backing up.
   */
  float cuspSeconds(float turnSpeed) const
  {
    const float ahead = std::min(topSpeed, turnSpeed);
    const float back = std::min(reverseSpeed, turnSpeed);
    const float v = 0.5f * (ahead + back);
    return std::max(shuttleSeconds, 0.0f) +
           v / (2.0f * std::max(brake, 0.05f)) +
           v / (2.0f * std::max(accel, 0.05f));
  }
};

} // namespace sfs::nav::vehicle
