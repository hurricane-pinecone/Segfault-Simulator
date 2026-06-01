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

// Compose the static projection config with a live camera view into a baked
// IsometricProjection snapshot. Call once per frame from the orchestrator; the
// camera pointers are read only transiently here, so the result is safe to hold
// after ECS component storage moves.
inline IsometricProjection makeProjection(const IsometricProjectionConfig& config,
                                          const ActiveCamera& camera)
{
  const float zoom = camera.camera ? camera.camera->zoom : 1.0f;

  return IsometricProjection{
      config.tileWidth,
      config.tileHeight,
      config.elevationStep,
      config.worldScale,
      zoom,
      camera.isoPosition(config.tileWidth, config.tileHeight, config.worldScale),
      config.screenCenter,
  };
}

} // namespace sfs
