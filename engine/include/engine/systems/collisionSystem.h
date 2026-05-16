#pragma once

#include "engine/components/colliderComponent.h"
#include "engine/components/elevationComponent.h"
#include "engine/components/rigidBodyComponent.h"
#include "engine/components/transformComponent.h"
#include "engine/ecs/registry.h"
#include "engine/ecs/system.h"

namespace sfs
{

class CollisionSystem : public System
{
public:
  CollisionSystem()
  {
    registerComponent<ColliderComponent>();
    registerComponent<RigidBodyComponent>();
    registerComponent<TransformComponent>();
  }

  ~CollisionSystem() = default;

  void update(double deltaTime) override
  {
    for (const auto& entity :
         registry->view<ColliderComponent, TransformComponent>())
    {
      auto& collider = entity.getComponent<ColliderComponent>();
      const auto& transform = entity.getComponent<TransformComponent>();

      collider.updateBounds(transform.position);
    }

    for (const auto& entity : getEntities())
    {
      auto& rb = entity.getComponent<RigidBodyComponent>();

      // If the entity is not moving, there's no point checking for collisions
      if (rb.velocity.x == 0.0f && rb.velocity.y == 0.0f)
        continue;

      auto& transform = entity.getComponent<TransformComponent>();
      auto& collider = entity.getComponent<ColliderComponent>();

      ColliderComponent previousCollider = collider;
      previousCollider.updateBounds(transform.previousPosition);

      if (const auto* hit = getCollision(entity, collider))
      {
        // Resolve X only if we were previously not overlapping on X.
        if (previousCollider.right() <= hit->left() && rb.velocity.x > 0.0f)
        {
          transform.position.x =
              hit->left() - collider.size.x - collider.offset.x;

          rb.velocity.x = 0.0f;
        }
        else if (previousCollider.left() >= hit->right() &&
                 rb.velocity.x < 0.0f)
        {
          transform.position.x = hit->right() - collider.offset.x;

          rb.velocity.x = 0.0f;
        }

        collider.updateBounds(transform.position);
      }

      if (const auto* hit = getCollision(entity, collider))
      {
        // Resolve Y only if we were previously not overlapping on Y.
        if (previousCollider.bottom() <= hit->top() && rb.velocity.y > 0.0f)
        {
          transform.position.y =
              hit->top() - collider.size.y - collider.offset.y;

          rb.velocity.y = 0.0f;
        }
        else if (previousCollider.top() >= hit->bottom() &&
                 rb.velocity.y < 0.0f)
        {
          transform.position.y = hit->bottom() - collider.offset.y;

          rb.velocity.y = 0.0f;
        }

        collider.updateBounds(transform.position);
      }
    }
  }

private:
  const ColliderComponent* getCollision(const Entity& entity,
                                        const ColliderComponent& collider)
  {
    int entityElevation = 0;

    if (entity.hasComponent<ElevationComponent>())
      entityElevation = entity.getComponent<ElevationComponent>().level;

    for (const auto& other : registry->view<ColliderComponent, SolidObject>())
    {
      if (entity == other)
        continue;

      int otherElevation = 0;

      if (other.hasComponent<ElevationComponent>())
        otherElevation = other.getComponent<ElevationComponent>().level;

      if (entityElevation != otherElevation)
        continue;

      const auto& otherCollider = other.getComponent<ColliderComponent>();

      if (collider.intersects(otherCollider))
        return &otherCollider;
    }

    return nullptr;
  }
};

} // namespace sfs
