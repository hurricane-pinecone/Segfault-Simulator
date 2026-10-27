#pragma once

#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/matrix_transform.hpp"
#include "glm/glm/ext/quaternion_float.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/gtc/quaternion.hpp"

namespace sfs
{

/**
 * An entity's placement in a Z-up 3D world: position, orientation and scale.
 * The 3D counterpart of TransformComponent; the 3D render path reads it and
 * game code writes it.
 *
 * @param glm::vec3 position - world position, default origin
 * @param glm::quat rotation - orientation, default identity
 * @param glm::vec3 scale - per-axis scale, default (1, 1, 1)
 */
struct Transform3DComponent
{
  glm::vec3 position{0.0f};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  glm::vec3 scale{1.0f};

  Transform3DComponent() = default;

  explicit Transform3DComponent(glm::vec3 position,
                                glm::quat rotation = {1.0f, 0.0f, 0.0f, 0.0f},
                                glm::vec3 scale = glm::vec3(1.0f))
      : position(position), rotation(rotation), scale(scale)
  {
  }

  /** Turn to face `yaw` radians around +Z (the common case on the ground). */
  void setYaw(float yaw)
  {
    rotation = glm::angleAxis(yaw, glm::vec3(0.0f, 0.0f, 1.0f));
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
};

} // namespace sfs
