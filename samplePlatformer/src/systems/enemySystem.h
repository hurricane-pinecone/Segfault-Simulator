#pragma once

#include "components/platformerComponents.h"
#include "config.h"
#include "engine/core/components/rigidBodyComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/system.h"
#include "glm/glm/ext/vector_float2.hpp"

namespace platformer
{

/**
 * Chase AI: each enemy walks horizontally toward the player. With no patrol
 * clamp, an enemy simply walks off a platform edge while chasing and gravity
 * drops it to the surface below (PlatformerPhysicsSystem owns the falling +
 * landing), so enemies pursue across and down through the level. Registered
 * before the physics system so the chase velocity is integrated the same frame.
 */
class EnemySystem : public sfs::System
{
protected:
  void create() override
  {
    registerComponent<sfs::TransformComponent>();
    registerComponent<sfs::RigidBodyComponent>();
    registerComponent<Enemy>();
  }

  void update(double /*deltaTime*/) override
  {
    if (!registry)
      return;

    glm::vec2 playerPos{0.0f, 0.0f};
    bool havePlayer = false;
    for (const auto& player :
         registry->view<PlayerTag, sfs::TransformComponent>())
    {
      playerPos = player.getComponent<sfs::TransformComponent>().position;
      havePlayer = true;
      break;
    }
    if (!havePlayer)
      return;

    for (const auto& entity : getEntities())
    {
      const auto& transform = entity.getComponent<sfs::TransformComponent>();
      auto& body = entity.getComponent<sfs::RigidBodyComponent>();

      const float dx = playerPos.x - transform.position.x;
      const float dir = dx > 4.0f ? 1.0f : (dx < -4.0f ? -1.0f : 0.0f);
      body.velocity.x = dir * ENEMY_SPEED;
    }
  }
};

} // namespace platformer
