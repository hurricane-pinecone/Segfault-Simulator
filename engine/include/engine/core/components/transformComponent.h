#pragma once

#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/matrix_transform.hpp"
#include "glm/glm/ext/quaternion_float.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/gtc/quaternion.hpp"
#include "glm/glm/trigonometric.hpp"

namespace sfs
{

/**
 * An entity's placement in the world: position, orientation and scale. Every
 * render style shares it. The world is Z-up: a 3D scene uses all three axes,
 * while flat and isometric scenes place things on their plane (x, y) through
 * the 2D helpers (position2D(), yaw()) and use z as the height above it.
 *
 * @param glm::vec3 position - world position, default origin
 * @param glm::quat rotation - orientation, default identity
 * @param glm::vec3 scale - per-axis scale, default (1, 1, 1)
 */
struct TransformComponent
{
  glm::vec3 position{0.0f};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  glm::vec3 scale{1.0f};

  TransformComponent() = default;

  explicit TransformComponent(glm::vec3 position,
                              glm::quat rotation = {1.0f, 0.0f, 0.0f, 0.0f},
                              glm::vec3 scale = glm::vec3(1.0f))
      : position(position), rotation(rotation), scale(scale)
  {
  }

  /**
   * Placement on the ground plane, for flat and isometric scenes: z is 0, the
   * scale's z 1, and `angle` turns it around +Z (radians).
   */
  explicit TransformComponent(glm::vec2 position,
                              glm::vec2 scale = glm::vec2(1.0f),
                              float angle = 0.0f)
      : position(position, 0.0f), scale(scale, 1.0f)
  {
    setYaw(angle);
  }

  /** Where it is on the ground plane (x, y). */
  glm::vec2 position2D() const { return {position.x, position.y}; }

  /** Move it on the ground plane, keeping its height (z). */
  void setPosition2D(glm::vec2 xy)
  {
    position.x = xy.x;
    position.y = xy.y;
  }

  /** Its scale on the ground plane (x, y). */
  glm::vec2 scale2D() const { return {scale.x, scale.y}; }

  /** Turn to face `yaw` radians around +Z (the common case on the ground). */
  void setYaw(float yaw)
  {
    rotation = glm::angleAxis(yaw, glm::vec3(0.0f, 0.0f, 1.0f));
  }

  /**
   * How far it's turned around +Z, in radians: a flat or isometric sprite's
   * rotation, or a 3D object's heading.
   */
  float yaw() const
  {
    const glm::vec3 forward = rotation * glm::vec3(1.0f, 0.0f, 0.0f);
    return glm::atan(forward.y, forward.x);
  }

  /** Local-to-world matrix: translate * rotate * scale. */
  glm::mat4 matrix() const
  {
    glm::mat4 m = glm::mat4_cast(rotation);
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    m[3] = glm::vec4(position, 1.0f);
    return m;
  }

  /**
   * The transform a translate * rotate * scale matrix was built from (the
   * inverse of matrix()). Shear, which no transform can hold, is dropped.
   */
  static TransformComponent fromMatrix(const glm::mat4& m)
  {
    const glm::vec3 scale{glm::length(glm::vec3(m[0])),
                          glm::length(glm::vec3(m[1])),
                          glm::length(glm::vec3(m[2]))};
    const glm::mat3 rotation{glm::vec3(m[0]) / scale.x,
                             glm::vec3(m[1]) / scale.y,
                             glm::vec3(m[2]) / scale.z};
    return TransformComponent(
        glm::vec3(m[3]), glm::normalize(glm::quat_cast(rotation)), scale);
  }
};

} // namespace sfs
