#pragma once

#include "components/rigidBodyComponent.h"
#include "components/transformComponent.h"
#include "ecs/system.h"

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
    for (auto entity : getEntities())
    {
      auto& transform = entity.getComponent<TransformComponent>();
      const auto rigidBody = entity.getComponent<RigidBodyComponent>();

      transform.position.x += rigidBody.velocity.x * deltaTime;
      transform.position.y += rigidBody.velocity.y * deltaTime;
      transform.rotation += 30.0 * deltaTime;
    }
  };
};
