#pragma once

#include "engine/components/colliderComponent.h"
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
    for (const auto& other : registry->view<ColliderComponent, SolidObject>())
    {
      if (entity == other)
        continue;

      const auto& otherCollider = other.getComponent<ColliderComponent>();

      if (collider.intersects(otherCollider))
        return &otherCollider;
    }

    return nullptr;
  }
};

} // namespace sfs
