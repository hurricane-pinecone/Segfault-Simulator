#pragma once

#include <engine/components/colliderComponent.h>
#include <engine/components/rigidBodyComponent.h>
#include <engine/components/transformComponent.h>
#include <engine/ecs/system.h>

namespace sfs
{

class MovementSystem : public System
{
public:
  MovementSystem()
  {
    registerComponent<TransformComponent>();
    registerComponent<RigidBodyComponent>();
  }

  ~MovementSystem() = default;

  void update(double deltaTime) override
  {
    for (const auto& entity : getEntities())
    {
      auto& rb = entity.getComponent<RigidBodyComponent>();
      auto& transform = entity.getComponent<TransformComponent>();

      // Allows collision system to check lest position
      transform.previousPosition = transform.position;
      transform.position += rb.velocity * static_cast<float>(deltaTime);

      if (entity.hasComponent<ColliderComponent>())
      {
        auto& collider = entity.getComponent<ColliderComponent>();

        collider.updateBounds(transform.position);
      }
    }
  }
};

} // namespace sfs
