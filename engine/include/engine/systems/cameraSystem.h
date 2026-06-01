#pragma once

#include <engine/components/cameraComponent.h>
#include <engine/components/transformComponent.h>
#include <engine/ecs/system.h>
#include <engine/rendering/util/isometric/camera.h>
#include <glm/glm/common.hpp>
#include <glm/glm/ext/vector_float2.hpp>

#include <engine/ecs/registry.h>

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

      auto target = registry->getEntity(camera.target);

      if (!target)
        continue;

      if (!target.hasComponent<TransformComponent>())
        continue;

      const auto& targetTransform = target.getComponent<TransformComponent>();

      glm::vec2 desiredPosition = targetTransform.position + camera.offset;

      float t =
          1.0f - std::exp(-camera.smoothing * static_cast<float>(deltaTime));

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
