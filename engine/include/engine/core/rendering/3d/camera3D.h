#pragma once

#include "engine/core/rendering/3d/aabb.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/quaternion_float.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

/** A ray in world space; direction is unit length. */
struct Ray3D
{
  glm::vec3 origin{0.0f};
  glm::vec3 direction{0.0f, 0.0f, -1.0f};

  glm::vec3 at(float distance) const { return origin + direction * distance; }

  /**
   * Distance along the ray to the horizontal plane Z = height.
   *
   * @return false if the ray is parallel to the plane or points away from it.
   */
  bool intersectPlaneZ(float height, float& outDistance) const;

  /**
   * Distance along the ray to where it enters `box` (0 if it starts inside).
   *
   * @return false if the ray misses the box or the box is behind it.
   */
  bool intersectAabb(const Aabb& box, float& outDistance) const;
};

/**
 * An orbit camera for a Z-up world: it looks at `target` from `distance` away,
 * turned `yaw` radians around +Z and raised `pitch` radians above the ground
 * plane. Perspective or orthographic. Projections map depth to [0, 1] (the
 * WebGPU / D3D / Vulkan clip convention).
 *
 * Plain data plus math: a game drives the fields from its own input, so free
 * look, follow cams and fixed angles are all just different ways of setting
 * them.
 */
struct Camera3D
{
  enum class Projection
  {
    Perspective,
    Orthographic,
  };

  glm::vec3 target{0.0f};
  float yaw = 0.0f;
  float pitch = 0.6f;
  float distance = 20.0f;

  Projection projection = Projection::Perspective;
  float fieldOfView = 0.8f; // vertical, radians (perspective)
  float orthoHeight = 20.0f; // visible world height (orthographic)
  float nearPlane = 0.1f;
  float farPlane = 500.0f;

  /** World position of the eye. */
  glm::vec3 position() const;

  /** Unit view direction (eye -> target). */
  glm::vec3 viewDirection() const;

  /** Unit forward along the ground (view direction with Z dropped). */
  glm::vec3 groundForward() const;

  /** Unit right along the ground, perpendicular to groundForward(). */
  glm::vec3 groundRight() const;

  /**
   * Look from `eye` along `forward` (any length): sets yaw and pitch from the
   * direction, and puts the target `focusDistance` ahead. There's no roll, so
   * the view stays level.
   */
  void setPose(const glm::vec3& eye, const glm::vec3& forward, float focusDistance);

  /**
   * The eye's orientation: +X along the view direction, +Z leaning up (the
   * convention camera entities use). With position(), the inverse of setPose.
   */
  glm::quat orientation() const;

  glm::mat4 viewMatrix() const;
  glm::mat4 projectionMatrix(float aspect) const;
  glm::mat4 viewProjection(float aspect) const;

  /**
   * World-space ray through a screen pixel (origin top-left, Y down), for
   * picking.
   */
  Ray3D screenRay(const glm::vec2& pixel, const glm::vec2& viewportSize) const;
};

} // namespace sfs
