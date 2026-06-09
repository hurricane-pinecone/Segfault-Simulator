#pragma once

#include "glm/glm/ext/matrix_clip_space.hpp" // ortho
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/matrix_transform.hpp" // lookAt
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/trigonometric.hpp"

namespace sfs
{

// Orthographic camera locked to a fixed isometric PITCH but free to orbit in
// YAW around a focus point. Orthographic projection keeps the flat isometric
// look (no perspective foreshortening) while the base-builder spins the view to
// see behind things. World is Y-up. Distance only sets the clip range, not the
// on-screen size (that's `zoom`, the ortho half-height).
struct OrthoOrbitCamera
{
  glm::vec3 focus{0.0f, 0.0f, 0.0f};
  float yaw = 0.785398f;  // radians, free to orbit
  float pitch = 0.61548f; // ~35.264deg, the true-isometric tilt (fixed)
  float zoom = 40.0f;  // ortho half-height, in world units (smaller = closer)
  float aspect = 1.0f; // viewport width / height
  float distance = 600.0f; // eye standoff; clip-range only under ortho

  glm::vec3 eye() const
  {
    const float cp = glm::cos(pitch);
    const float sp = glm::sin(pitch);
    const glm::vec3 dir{cp * glm::sin(yaw), sp, cp * glm::cos(yaw)};
    return focus + dir * distance;
  }

  glm::mat4 view() const
  {
    return glm::lookAt(eye(), focus, glm::vec3{0.0f, 1.0f, 0.0f});
  }

  glm::mat4 proj() const
  {
    const float hh = zoom;
    const float hw = zoom * aspect;
    return glm::ortho(-hw, hw, -hh, hh, 0.1f, distance * 2.0f + 2000.0f);
  }

  glm::mat4 viewProj() const { return proj() * view(); }

  // Horizontal forward/right in world space for the current yaw, so movement
  // can follow the camera as it orbits (W = into the screen regardless of
  // angle).
  glm::vec3 forward() const
  {
    return glm::vec3{-glm::sin(yaw), 0.0f, -glm::cos(yaw)};
  }
  glm::vec3 right() const
  {
    return glm::vec3{glm::cos(yaw), 0.0f, -glm::sin(yaw)};
  }
};

} // namespace sfs
