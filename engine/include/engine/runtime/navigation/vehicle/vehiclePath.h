#pragma once

#include "engine/runtime/navigation/geometry.h"

#include <vector>

namespace sfs::nav::vehicle
{

/**
 * A stretch of a path: `length` metres in one gear (+1 forward, -1 backing
 * up), its curvature (1/m, + turning the vehicle left, as the wheel is)
 * going linearly from k0 to k1: a straight, an arc or a clothoid.
 */
struct Segment
{
  int gear = 1;
  float length = 0.0f;
  float k0 = 0.0f, k1 = 0.0f;
};

/** A point of a sampled path (every few centimetres). */
struct PathPoint
{
  float s = 0.0f; // metres along (driven either way)
  Pose2 pose;
  float curvature = 0.0f; // 1/m, the wheel's (+ left)
  int gear = 1;
  float hitch = 0.0f; // a trailer's angle on its hitch here
  float speed = 0.0f; // m/s, its speed profile (unsigned)
  float time = 0.0f;  // s from the start, by the profile
  int leg = 0;        // a caller's tag (which leg of its trip)
};

/** A path to follow: its segments from `start`, and their samples. */
struct VehiclePath
{
  Pose2 start;
  float startHitch = 0.0f;
  std::vector<Segment> segments;
  std::vector<PathPoint> points;

  bool empty() const { return points.size() < 2; }
  float length() const { return points.empty() ? 0.0f : points.back().s; }
  float duration() const { return points.empty() ? 0.0f : points.back().time; }
  Pose2 end() const { return points.empty() ? start : points.back().pose; }
  int cusps() const
  {
    int n = 0;
    for (std::size_t i = 1; i < segments.size(); ++i)
      n += segments[i].gear != segments[i - 1].gear;
    return n;
  }
};

} // namespace sfs::nav::vehicle
