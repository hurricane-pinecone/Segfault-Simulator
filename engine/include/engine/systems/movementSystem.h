#pragma once

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
  };
  ~MovementSystem() = default;

  void update(double deltaTime) override
  {
    for (const auto& entity : getEntities())
    {
      auto& transform = entity.getComponent<TransformComponent>();
      const auto rigidBody = entity.getComponent<RigidBodyComponent>();

      transform.position.x += rigidBody.velocity.x * deltaTime;
      transform.position.y += rigidBody.velocity.y * deltaTime;
      transform.rotation += transform.rotation * deltaTime;
    }
  };
};

} // namespace sfs
