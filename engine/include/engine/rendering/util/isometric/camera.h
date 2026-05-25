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

} // namespace sfs
