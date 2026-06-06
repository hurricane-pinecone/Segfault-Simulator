#pragma once

#include "components/platformerComponents.h"
#include "config.h"
#include "engine/core/components/boxCollider2D.h"
#include "engine/core/components/rigidBodyComponent.h"
#include "engine/core/components/tags/solidObject.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/system.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/vector_float2.hpp"

#include <vector>

namespace platformer
{

/**
 * Side-on platformer physics: a game-local sfs::System (no engine changes).
 * Applies gravity to every dynamic body (TransformComponent + RigidBody +
 * sfs::BoxCollider2D + PlatformerBody), integrates velocity, and resolves AABB
 * collisions against static sfs::SolidObject platforms one axis at a time,
 * setting onGround when a body lands on top of a platform.
 */
class PlatformerPhysicsSystem : public sfs::System
{
protected:
  void create() override
  {
    registerComponent<sfs::TransformComponent>();
    registerComponent<sfs::RigidBodyComponent>();
    registerComponent<sfs::BoxCollider2D>();
    registerComponent<PlatformerBody>();
  }

  void update(double deltaTime) override
  {
    if (!registry)
      return;

    const float dt = static_cast<float>(deltaTime);

    // Snapshot the static platforms once.
    struct Box
    {
      glm::vec2 center;
      glm::vec2 half;
    };
    std::vector<Box> solids;
    for (const auto& entity : registry->view<sfs::SolidObject,
                                             sfs::TransformComponent,
                                             sfs::BoxCollider2D>())
    {
      solids.push_back({entity.getComponent<sfs::TransformComponent>().position,
                        entity.getComponent<sfs::BoxCollider2D>().half});
    }

    for (const auto& entity : getEntities())
    {
      auto& transform = entity.getComponent<sfs::TransformComponent>();
      auto& body = entity.getComponent<sfs::RigidBodyComponent>();
      auto& state = entity.getComponent<PlatformerBody>();
      const glm::vec2 half = entity.getComponent<sfs::BoxCollider2D>().half;

      body.velocity.y += GRAVITY * dt;
      state.onGround = false;

      // Move and resolve on X, then Y, so a body slides along walls/floors.
      transform.position.x += body.velocity.x * dt;
      for (const auto& solid : solids)
        resolve(transform.position,
                half,
                body.velocity,
                solid.center,
                solid.half,
                true,
                state);

      transform.position.y += body.velocity.y * dt;
      for (const auto& solid : solids)
        resolve(transform.position,
                half,
                body.velocity,
                solid.center,
                solid.half,
                false,
                state);
    }
  }

private:
  static bool overlaps(const glm::vec2& ac,
                       const glm::vec2& ah,
                       const glm::vec2& bc,
                       const glm::vec2& bh)
  {
    return glm::abs(ac.x - bc.x) < ah.x + bh.x &&
           glm::abs(ac.y - bc.y) < ah.y + bh.y;
  }

  static void resolve(glm::vec2& pos,
                      const glm::vec2& half,
                      glm::vec2& velocity,
                      const glm::vec2& solidCenter,
                      const glm::vec2& solidHalf,
                      bool axisX,
                      PlatformerBody& state)
  {
    if (!overlaps(pos, half, solidCenter, solidHalf))
      return;

    if (axisX)
    {
      if (velocity.x > 0.0f)
        pos.x = solidCenter.x - solidHalf.x - half.x;
      else if (velocity.x < 0.0f)
        pos.x = solidCenter.x + solidHalf.x + half.x;
      velocity.x = 0.0f;
    }
    else
    {
      if (velocity.y > 0.0f) // moving down: landed on top
      {
        pos.y = solidCenter.y - solidHalf.y - half.y;
        state.onGround = true;
      }
      else if (velocity.y < 0.0f) // moving up: bonked head
      {
        pos.y = solidCenter.y + solidHalf.y + half.y;
      }
      velocity.y = 0.0f;
    }
  }
};

} // namespace platformer
