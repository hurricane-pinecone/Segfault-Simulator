#pragma once

#include "engine/core/components/rigidBody2DComponent.h"
#include "engine/core/components/tags/solidObject.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/components/worldCollider.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/system.h"

#include <cmath>

namespace sfs
{

// Solid world collision runs on WorldCollider (the ground footprint), not
// BoxCollider2D (which is only a billboard / bullet hit box).
class CollisionSystem : public System
{
public:
  CollisionSystem()
  {
    registerComponent<WorldCollider>();
    registerComponent<RigidBody2DComponent>();
    registerComponent<TransformComponent>();
  }

  ~CollisionSystem() = default;

  void update(double deltaTime) override
  {
    for (const auto& entity :
         registry->view<WorldCollider, TransformComponent>())
    {
      auto& collider = entity.getComponent<WorldCollider>();
      const auto& transform = entity.getComponent<TransformComponent>();

      collider.updateBounds(transform.position2D());
    }

    for (auto& entity : getEntities())
    {
      auto& rb = entity.getComponent<RigidBody2DComponent>();

      // If the entity is not moving, there's no point checking for collisions
      if (rb.velocity.x == 0.0f && rb.velocity.y == 0.0f)
        continue;

      auto& transform = entity.getComponent<TransformComponent>();
      auto& collider = entity.getComponent<WorldCollider>();

      WorldCollider previousCollider = collider;
      previousCollider.updateBounds(rb.previousPosition);

      if (const auto* hit = getCollision(entity, collider))
      {
        // Resolve X only if we were previously not overlapping on X.
        if (previousCollider.right() <= hit->left() && rb.velocity.x > 0.0f)
        {
          transform.position.x =
              hit->left() - collider.worldSize().x - collider.worldOffset().x;

          rb.velocity.x = 0.0f;
        }
        else if (previousCollider.left() >= hit->right() &&
                 rb.velocity.x < 0.0f)
        {
          transform.position.x = hit->right() - collider.worldOffset().x;

          rb.velocity.x = 0.0f;
        }

        collider.updateBounds(transform.position2D());
      }

      if (const auto* hit = getCollision(entity, collider))
      {
        // Resolve Y only if we were previously not overlapping on Y.
        if (previousCollider.bottom() <= hit->top() && rb.velocity.y > 0.0f)
        {
          transform.position.y =
              hit->top() - collider.worldSize().y - collider.worldOffset().y;

          rb.velocity.y = 0.0f;
        }
        else if (previousCollider.top() >= hit->bottom() &&
                 rb.velocity.y < 0.0f)
        {
          transform.position.y = hit->bottom() - collider.worldOffset().y;

          rb.velocity.y = 0.0f;
        }

        collider.updateBounds(transform.position2D());
      }
    }
  }

private:
  const WorldCollider* getCollision(const Entity& entity,
                                    const WorldCollider& collider)
  {
    // Only things at the same height (the transform's z) meet.
    const auto heightOf = [](const Entity& e)
    {
      return e.hasComponent<TransformComponent>()
                 ? std::lround(e.getComponent<TransformComponent>().position.z)
                 : 0L;
    };
    const long entityElevation = heightOf(entity);

    for (const auto& other : registry->view<WorldCollider, SolidObject>())
    {
      if (entity == other)
        continue;

      if (entityElevation != heightOf(other))
        continue;

      const auto& otherCollider = other.getComponent<WorldCollider>();

      if (collider.intersects(otherCollider))
        return &otherCollider;
    }

    return nullptr;
  }
};

} // namespace sfs
