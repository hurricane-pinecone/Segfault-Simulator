#pragma once

#include "engine/core/components/cameraComponent.h"
#include "engine/core/components/transformComponent.h"
#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

// A camera resolved to plain component pointers, returned by CameraSystem. The
// world position it yields is projection-agnostic, so both the isometric and
// flat-2D render paths build their projection from it.
struct ActiveCamera
{
  const CameraComponent* camera = nullptr;
  const TransformComponent* transform = nullptr;

  // World-space point the camera is centred on: the followed (smoothed)
  // position plus the static offset plus the transient shake displacement.
  glm::vec2 getCameraPosition() const
  {
    if (!camera || !transform)
      return {0.0f, 0.0f};

    return transform->position + camera->offset + camera->shakeOffset();
  }
};

} // namespace sfs
