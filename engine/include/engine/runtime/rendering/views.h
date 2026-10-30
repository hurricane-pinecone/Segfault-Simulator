#pragma once

#include "glm/glm/ext/quaternion_float.hpp"
#include "glm/glm/ext/quaternion_trigonometric.hpp"
#include "glm/glm/trigonometric.hpp"

namespace sfs::views
{

/**
 * A camera entity's rotation (CameraComponent: it looks along its +X, +Z up)
 * heading `yaw` degrees around +Z and looking `pitch` degrees down.
 */
inline glm::quat looking(float yaw, float pitch)
{
  return glm::angleAxis(glm::radians(yaw), glm::vec3(0.0f, 0.0f, 1.0f)) *
         glm::angleAxis(glm::radians(pitch), glm::vec3(0.0f, 1.0f, 0.0f));
}

/**
 * Flat 2D: the XY plane seen straight on from +Z, +X right and +Y up the
 * screen. With an orthographic camera, what a 2D game draws in.
 */
inline glm::quat flat() { return looking(90.0f, 90.0f); }

/**
 * Isometric: looking down 30 degrees, so the ground's square tiles show as
 * 2:1 diamonds, from the +X+Y corner (by default) toward -X-Y. With an
 * orthographic camera.
 */
inline glm::quat isometric(float yaw = 225.0f) { return looking(yaw, 30.0f); }

} // namespace sfs::views
