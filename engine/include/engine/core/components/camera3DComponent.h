#pragma once

#include "engine/core/rendering/3d/camera3D.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"

namespace sfs
{

/**
 * Makes the entity a camera the scene can be seen through. The entity's
 * Transform3DComponent places it, looking along its +X axis with +Z up (the
 * way setYaw faces things). Move the entity to move the view.
 *
 * A RenderSystem3D renders through the first active camera entity, in place of
 * its own camera(), unless told not to (the editor looks around freely while
 * editing). Roll is ignored: the view stays level.
 *
 * @param bool active - whether this camera can be the scene's view; turn one
 *                      off and another on to switch cameras
 * @param bool orthographic - parallel projection instead of perspective
 * @param float fieldOfView - vertical, in degrees (perspective)
 * @param float orthoHeight - visible world height (orthographic)
 * @param float nearPlane - nearest visible distance
 * @param float farPlane - farthest visible distance
 * @param float focusDistance - how far ahead the camera's attention is: the
 *                              sun's shadows are sharpest there, and the point
 *                              lights nearest it are kept when there are too
 *                              many
 */
struct Camera3DComponent
{
  bool active = true;
  bool orthographic = false;
  [[sfs::range(10, 120)]] float fieldOfView = 46.0f;
  [[sfs::range(1, 200)]] float orthoHeight = 20.0f;
  [[sfs::range(0.01, 10)]] float nearPlane = 0.1f;
  [[sfs::range(10, 5000)]] float farPlane = 500.0f;
  [[sfs::range(1, 200)]] float focusDistance = 20.0f;

  /** Point `camera` the way this camera, with world matrix `world`, looks. */
  void applyTo(Camera3D& camera, const glm::mat4& world) const
  {
    camera.setPose(glm::vec3(world[3]), glm::vec3(world[0]), focusDistance);
    camera.projection =
        orthographic ? Camera3D::Projection::Orthographic : Camera3D::Projection::Perspective;
    camera.fieldOfView = glm::radians(fieldOfView);
    camera.orthoHeight = orthoHeight;
    camera.nearPlane = nearPlane;
    camera.farPlane = farPlane;
  }
};

} // namespace sfs
