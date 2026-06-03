#pragma once

#include "engine/components/cameraComponent.h"
#include "engine/components/transformComponent.h"
#include "engine/rendering/util/isometric/geometry.h"
#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

struct ActiveCamera
{
  const CameraComponent* camera = nullptr;
  const TransformComponent* transform = nullptr;

  glm::vec2 getCameraPosition() const
  {

    if (!camera || !transform)
      return {0.0f, 0.0f};

    return transform->position + camera->offset;
  }

  glm::vec2 isoPosition(int tileWidth, int tileHeight, float worldScale) const
  {
    return gridToIsometric(
        getCameraPosition(), tileWidth, tileHeight, worldScale);
  }
};

// Bakes the camera into plain values, so the result stays valid even after ECS
// component storage moves and invalidates the camera's component pointers.
inline IsometricProjection makeProjection(const IsometricProjectionConfig& config,
                                          const ActiveCamera& camera)
{
  const float zoom = camera.camera ? camera.camera->zoom : 1.0f;

  IsometricProjection projection;
  projection.tileWidth = config.tileWidth;
  projection.tileHeight = config.tileHeight;
  projection.elevationStep = config.elevationStep;
  projection.worldScale = config.worldScale;
  projection.zoom = zoom;
  projection.cameraIso =
      camera.isoPosition(config.tileWidth, config.tileHeight, config.worldScale);
  projection.screenCenter = config.screenCenter;

  return projection;
}

} // namespace sfs
