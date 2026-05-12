#pragma once

#include "engine/systems/isometricRenderSystem.h"
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

      transform.previousPosition = transform.position;
      transform.position += rb.velocity * static_cast<float>(deltaTime);

      if (entity.hasComponent<ElevationComponent>())
      {
        auto& elevation = entity.getComponent<ElevationComponent>();
        elevation.level = getElevationAt(transform.position);
      }

      if (entity.hasComponent<ColliderComponent>())
      {
        auto& collider = entity.getComponent<ColliderComponent>();
        collider.updateBounds(transform.position);
      }
    }
  }

private:
  int getElevationAt(const glm::vec2& position) const
  {
    int x = static_cast<int>(std::floor(position.x));
    int y = static_cast<int>(std::floor(position.y));

    if (x < 0 || x >= 25 || y < 0 || y >= 25)
      return 0;

    if (y < 10)
      return 10 - y;

    return 0;
  }
};

} // namespace sfs
