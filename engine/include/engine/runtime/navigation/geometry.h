#pragma once

#include "glm/glm/common.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>

/**
 * sfs::nav: navigation for AI agents (NavigationSystem, NavAgentComponent).
 * This folder holds what every agent kind shares (the world it plans in:
 * Space, CostLayer; the search interface and its registry; plan states;
 * Traffic and the coarse graph); each kind's own planning lives in its
 * folder (vehicle/ now; walker/ later) and depends on this, never the other
 * way round.
 *
 * Frames: the ground is the XY plane, Z up; a heading is radians about +Z,
 * 0 facing +X.
 */
namespace sfs::nav
{

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 2.0f * kPi;

/** An angle brought into (-pi, pi]. */
inline float wrapAngle(float a)
{
  // (Usually a turn or less off already: no division.)
  if (a > -kPi && a <= kPi)
    return a;
  if (a > kPi && a <= 3.0f * kPi)
    return a - kTwoPi;
  if (a <= -kPi && a > -3.0f * kPi)
    return a + kTwoPi;
  a = std::fmod(a + kPi, kTwoPi);
  if (a <= 0.0f)
    a += kTwoPi;
  return a - kPi;
}
/** An angle brought into [0, 2 pi). */
inline float wrapPositive(float a)
{
  if (a >= 0.0f && a < kTwoPi)
    return a;
  if (a < 0.0f && a >= -kTwoPi)
    return a + kTwoPi < kTwoPi ? a + kTwoPi : 0.0f;
  a = std::fmod(a, kTwoPi);
  return a < 0.0f ? a + kTwoPi : a;
}
inline glm::vec2 unit(float angle)
{
  return {std::cos(angle), std::sin(angle)};
}

/** Where an agent's reference point is and which way it faces. */
struct Pose2
{
  glm::vec2 position{0.0f};
  float heading = 0.0f;
};

/**
 * One of an agent's boxes, in its reference point's frame: from `back` to
 * `front` along it (back < front), `half` either side of a middle line
 * `shift` to its left, from `bottom` to `top` above the ground.
 */
struct Box3
{
  float back = -1.0f, front = 1.0f;
  float half = 1.0f, shift = 0.0f;
  float bottom = 0.0f, top = 2.0f;
};

/** An axis-aligned rectangle of ground. */
struct Rect
{
  glm::vec2 lo{0.0f}, hi{0.0f};
  bool contains(glm::vec2 p) const
  {
    return p.x >= lo.x && p.y >= lo.y && p.x <= hi.x && p.y <= hi.y;
  }
};

} // namespace sfs::nav
