#pragma once

#include <engine/core/components/cameraComponent.h>
#include <engine/core/components/cameraFollowComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/ecs/system.h>
#include <engine/runtime/rendering/util/activeCamera.h>
#include <glm/glm/common.hpp>
#include <glm/glm/exponential.hpp>
#include <glm/glm/ext/vector_float3.hpp>

#include <engine/core/ecs/registry.h>

namespace sfs
{

/**
 * Moves cameras that follow something (CameraFollowComponent): eases each one
 * toward its target and advances its shake. The same in every render style.
 * Also resolves the scene's active camera (activeCamera()).
 */
class CameraSystem : public System
{
public:
  CameraSystem()
  {
    registerComponent<TransformComponent>();
    registerComponent<CameraFollowComponent>();
  }

  void update(double deltaTime) override
  {
    for (auto& cameraEntity : getEntities())
    {
      auto& cameraTransform = cameraEntity.getComponent<TransformComponent>();
      auto& follow = cameraEntity.getComponent<CameraFollowComponent>();

      // Shake is independent of following, so advance it before the target
      // checks below can `continue` past the rest of the loop.
      follow.updateShake(static_cast<float>(deltaTime));

      auto target = registry->getEntity(follow.target);

      if (!target)
        continue;

      if (!target.hasComponent<TransformComponent>())
        continue;

      const auto& targetTransform = target.getComponent<TransformComponent>();

      const glm::vec3 desiredPosition =
          targetTransform.position + follow.offset;

      float t =
          1.0f - glm::exp(-follow.smoothing * static_cast<float>(deltaTime));

      cameraTransform.position =
          glm::mix(cameraTransform.position, desiredPosition, t);
    }
  }

  // The scene's active camera (see findActiveCamera).
  ActiveCamera activeCamera() const
  {
    return registry ? findActiveCamera(*registry) : ActiveCamera{};
  }
};

} // namespace sfs
