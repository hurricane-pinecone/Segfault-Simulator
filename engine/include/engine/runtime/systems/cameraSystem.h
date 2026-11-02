#pragma once

#include <engine/core/components/cameraComponent.h>
#include <engine/core/components/cameraFollowComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/ecs/system.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/runtime/rendering/util/activeCamera.h>
#include <glm/glm/common.hpp>
#include <glm/glm/exponential.hpp>
#include <glm/glm/ext/vector_float3.hpp>
#include <glm/glm/matrix.hpp>

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

      // In the world, so a parented camera or target follows as well.
      const glm::vec3 desired =
          hierarchy::worldPosition(target) + follow.offset;
      const glm::vec3 current = hierarchy::worldPosition(cameraEntity);

      float t =
          1.0f - glm::exp(-follow.smoothing * static_cast<float>(deltaTime));

      const glm::vec3 next = glm::mix(current, desired, t);
      cameraTransform.position =
          hierarchy::parentOf(cameraEntity).isValid()
              ? glm::vec3(
                    glm::inverse(hierarchy::parentWorldMatrix(cameraEntity)) *
                    glm::vec4(next, 1.0f))
              : next;
    }
  }

  // The scene's active camera (see findActiveCamera), or an invalid entity.
  Entity activeCamera() const
  {
    return registry ? findActiveCamera(*registry) : Entity{};
  }
};

} // namespace sfs
