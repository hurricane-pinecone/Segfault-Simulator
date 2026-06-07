#pragma once

#include "engine/core/rendering/projection/isometricProjection.h"
#include "engine/core/rendering/util/isometric/geometry.h"
#include "engine/runtime/rendering/util/activeCamera.h"
#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

// Bakes the (projection-agnostic) ActiveCamera into an isometric projection.
// The result holds plain values, so it stays valid even after ECS component
// storage moves and invalidates the camera's component pointers.
inline IsometricProjection
makeProjection(const IsometricProjectionConfig& config,
               const ActiveCamera& camera)
{
  const float zoom = camera.camera ? camera.camera->zoom : 1.0f;

  IsometricProjection projection;
  projection.tileWidth = config.tileWidth;
  projection.tileHeight = config.tileHeight;
  projection.elevationStep = config.elevationStep;
  projection.worldScale = config.worldScale;
  projection.zoom = zoom;
  projection.cameraIso = gridToIsometric(camera.getCameraPosition(),
                                         config.tileWidth,
                                         config.tileHeight,
                                         config.worldScale);
  projection.screenCenter = config.screenCenter;

  return projection;
}

} // namespace sfs
