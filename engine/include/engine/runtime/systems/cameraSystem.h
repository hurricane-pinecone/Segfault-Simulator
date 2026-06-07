#pragma once

#include <engine/core/components/cameraComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/ecs/system.h>
#include <engine/runtime/rendering/util/activeCamera.h>
#include <glm/glm/common.hpp>
#include <glm/glm/exponential.hpp>
#include <glm/glm/ext/vector_float2.hpp>

#include <engine/core/ecs/registry.h>

namespace sfs
{

class CameraSystem : public System
{
public:
  CameraSystem()
  {
    registerComponent<TransformComponent>();
    registerComponent<CameraComponent>();
  }

  void update(double deltaTime) override
  {
    for (auto& cameraEntity : getEntities())
    {
      auto& cameraTransform = cameraEntity.getComponent<TransformComponent>();
      auto& camera = cameraEntity.getComponent<CameraComponent>();

      // Shake is independent of following, so advance it before the target
      // checks below can `continue` past the rest of the loop.
      camera.updateShake(static_cast<float>(deltaTime));

      auto target = registry->getEntity(camera.target);

      if (!target)
        continue;

      if (!target.hasComponent<TransformComponent>())
        continue;

      const auto& targetTransform = target.getComponent<TransformComponent>();

      glm::vec2 desiredPosition = targetTransform.position + camera.offset;

      float t =
          1.0f - glm::exp(-camera.smoothing * static_cast<float>(deltaTime));

      cameraTransform.position =
          glm::mix(cameraTransform.position, desiredPosition, t);
    }
  }

  // The active (first) camera, or an empty ActiveCamera when none exists.
  ActiveCamera activeCamera() const
  {
    const auto& cameras = getEntities();

    if (cameras.empty())
      return {};

    const auto& cameraEntity = cameras.front();

    return {
        &cameraEntity.getComponent<CameraComponent>(),
        &cameraEntity.getComponent<TransformComponent>(),
    };
  }
};

} // namespace sfs
