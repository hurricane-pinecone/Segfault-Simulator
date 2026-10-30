#pragma once

#include "engine/core/components/cameraComponent.h"
#include "engine/core/components/cameraFollowComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

// A camera resolved to plain component pointers, returned by CameraSystem.
struct ActiveCamera
{
  const CameraComponent* camera = nullptr;
  const TransformComponent* transform = nullptr;
  // Null when the camera doesn't follow anything.
  const CameraFollowComponent* follow = nullptr;

  // How much the camera magnifies (CameraComponent::zoom); 1 without one.
  float zoom() const { return camera ? camera->zoom : 1.0f; }

  // World-space point on the ground plane the camera is centred on: its
  // position, plus a follow's static offset and transient shake displacement.
  glm::vec2 getCameraPosition() const
  {
    if (!transform)
      return {0.0f, 0.0f};

    glm::vec2 position = transform->position2D();
    if (follow)
      position += glm::vec2(follow->offset) + follow->shakeOffset();
    return position;
  }
};

// The scene's first active camera (CameraComponent + TransformComponent, in
// id order, as RenderSystem picks it), or an empty ActiveCamera when none.
inline ActiveCamera findActiveCamera(Registry& registry)
{
  for (const auto& entity :
       registry.view<CameraComponent, TransformComponent>())
  {
    const auto& camera = entity.getComponent<CameraComponent>();
    if (!camera.active)
      continue;

    return {
        &camera,
        &entity.getComponent<TransformComponent>(),
        entity.hasComponent<CameraFollowComponent>()
            ? &entity.getComponent<CameraFollowComponent>()
            : nullptr,
    };
  }

  return {};
}

} // namespace sfs
