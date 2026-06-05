#pragma once

#include "engine/core/components/elevationComponent.h"
#include "engine/core/components/rigidBodyComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/components/worldCollider.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/system.h"

namespace sfs
{

// Solid world collision runs on WorldCollider (the ground footprint), not
// ScreenSpaceCollider (which is only a billboard / bullet hit box).
class CollisionSystem : public System
{
public:
  CollisionSystem()
  {
    registerComponent<WorldCollider>();
    registerComponent<RigidBodyComponent>();
    registerComponent<TransformComponent>();
  }

  ~CollisionSystem() = default;

  // Draw the debug collider overlay this frame (IsometricRenderSystem renders
  // it; the scene gates its call on this).
  bool debugDraw() const { return m_debugDraw; }

  std::vector<ModuleSetting> settings() override
  {
    return {settings::boolean(
        "Show colliders",
        [this] { return m_debugDraw; },
        [this](bool value) { m_debugDraw = value; })};
  }

  void update(double deltaTime) override
  {
    for (const auto& entity :
         registry->view<WorldCollider, TransformComponent>())
    {
      auto& collider = entity.getComponent<WorldCollider>();
      const auto& transform = entity.getComponent<TransformComponent>();

      collider.updateBounds(transform.position);
    }

    for (auto& entity : getEntities())
    {
      auto& rb = entity.getComponent<RigidBodyComponent>();

      // If the entity is not moving, there's no point checking for collisions
      if (rb.velocity.x == 0.0f && rb.velocity.y == 0.0f)
        continue;

      auto& transform = entity.getComponent<TransformComponent>();
      auto& collider = entity.getComponent<WorldCollider>();

      WorldCollider previousCollider = collider;
      previousCollider.updateBounds(transform.previousPosition);

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

        collider.updateBounds(transform.position);
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

        collider.updateBounds(transform.position);
      }
    }
  }

private:
  const WorldCollider* getCollision(const Entity& entity,
                                    const WorldCollider& collider)
  {
    int entityElevation = 0;

    if (entity.hasComponent<ElevationComponent>())
      entityElevation = entity.getComponent<ElevationComponent>().level;

    for (const auto& other : registry->view<WorldCollider, SolidObject>())
    {
      if (entity == other)
        continue;

      int otherElevation = 0;

      if (other.hasComponent<ElevationComponent>())
        otherElevation = other.getComponent<ElevationComponent>().level;

      if (entityElevation != otherElevation)
        continue;

      const auto& otherCollider = other.getComponent<WorldCollider>();

      if (collider.intersects(otherCollider))
        return &otherCollider;
    }

    return nullptr;
  }

  bool m_debugDraw = false;
};

} // namespace sfs
