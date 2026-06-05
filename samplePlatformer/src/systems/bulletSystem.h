#pragma once

#include "components/platformerComponents.h"
#include "config.h"
#include "engine/core/components/rigidBodyComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/system.h"
#include "engine/core/particles/particleEngine.h"
#include "glm/glm/ext/vector_float2.hpp"

#include <cmath>
#include <functional>
#include <vector>

namespace platformer
{

/**
 * Advances bullets, expires them, and tests each against the live enemies. On a
 * hit it sprays a blood burst at the impact (through the injected ParticleEngine
 * -- the engine's generic particle module on the flat path), damages the enemy,
 * and removes the bullet; an enemy at zero health is destroyed.
 */
class BulletSystem : public sfs::System
{
public:
  // The particle engine bursts are fired through (the FlatRenderSystem's
  // Particles module). Must have "blood" / "blood_stain" / "gore" registered.
  void setBlood(sfs::ParticleEngine* blood) { m_blood = blood; }

  // Called at an enemy's death position (for score, screen shake, death flash).
  void setOnKill(std::function<void(glm::vec2)> onKill)
  {
    m_onKill = std::move(onKill);
  }

protected:
  void create() override
  {
    registerComponent<sfs::TransformComponent>();
    registerComponent<Bullet>();
  }

  void update(double deltaTime) override
  {
    if (!registry)
      return;

    const float dt = static_cast<float>(deltaTime);
    const glm::vec2 bulletHalf{BULLET_HIT, BULLET_HIT};

    std::vector<sfs::Entity> deadBullets;
    std::vector<sfs::Entity> deadEnemies;

    auto enemies =
        registry->view<Enemy, sfs::TransformComponent, BoxCollider>();

    for (const auto& bulletEntity : getEntities())
    {
      auto& transform = bulletEntity.getComponent<sfs::TransformComponent>();
      auto& bullet = bulletEntity.getComponent<Bullet>();

      transform.position += bullet.velocity * dt;
      bullet.life -= dt;

      if (bullet.life <= 0.0f)
      {
        deadBullets.push_back(bulletEntity);
        continue;
      }

      for (const auto& enemyEntity : enemies)
      {
        auto& enemy = enemyEntity.getComponent<Enemy>();
        if (enemy.health <= 0.0f)
          continue;

        const auto& enemyTransform =
            enemyEntity.getComponent<sfs::TransformComponent>();
        const glm::vec2 enemyHalf = enemyEntity.getComponent<BoxCollider>().half;

        if (overlaps(transform.position, bulletHalf, enemyTransform.position,
                     enemyHalf))
        {
          enemy.health -= BULLET_DAMAGE;

          // Upward knockback pop as a hit reaction (patrol overwrites X next
          // frame, so only the vertical pop persists).
          enemyEntity.getComponent<sfs::RigidBodyComponent>().velocity.y =
              -ENEMY_KNOCKBACK;

          if (m_blood)
          {
            // Spray at the impact, plus a lingering stain on the actual platform
            // beneath the hit -- so blood only sticks where there's a surface,
            // not in mid-air when an enemy is shot while falling or off an edge.
            m_blood->spawnBurst("blood", transform.position, 0.0f);
            float surfaceY = 0.0f;
            if (platformTopBelow(transform.position, surfaceY))
              m_blood->spawnBurst("blood_stain",
                                  {transform.position.x, surfaceY}, 0.0f);
          }

          deadBullets.push_back(bulletEntity);

          if (enemy.health <= 0.0f)
          {
            // Gory death: a big gib burst + a notification for shake/score/flash.
            if (m_blood)
              m_blood->spawnBurst("gore", enemyTransform.position, 0.0f);
            if (m_onKill)
              m_onKill(enemyTransform.position);
            deadEnemies.push_back(enemyEntity);
          }
          break;
        }
      }
    }

    for (const auto& bullet : deadBullets)
      registry->destroyEntity(bullet);
    for (const auto& enemy : deadEnemies)
      registry->destroyEntity(enemy);
  }

private:
  static bool overlaps(const glm::vec2& ac, const glm::vec2& ah,
                       const glm::vec2& bc, const glm::vec2& bh)
  {
    return std::fabs(ac.x - bc.x) < ah.x + bh.x &&
           std::fabs(ac.y - bc.y) < ah.y + bh.y;
  }

  // Top surface (smallest screen Y, i.e. highest) of the nearest Solid platform
  // directly under `point`, if any. Lets a blood stain land on a real surface
  // rather than in mid-air.
  bool platformTopBelow(const glm::vec2& point, float& outTopY)
  {
    bool found = false;
    float best = 0.0f;

    for (const auto& solid :
         registry->view<Solid, sfs::TransformComponent, BoxCollider>())
    {
      const glm::vec2 center =
          solid.getComponent<sfs::TransformComponent>().position;
      const glm::vec2 half = solid.getComponent<BoxCollider>().half;

      if (point.x < center.x - half.x || point.x > center.x + half.x)
        continue;

      const float top = center.y - half.y; // Y grows downward
      if (top < point.y - 1.0f)
        continue; // platform must be at or below the hit

      if (!found || top < best)
      {
        best = top;
        found = true;
      }
    }

    outTopY = best;
    return found;
  }

  sfs::ParticleEngine* m_blood = nullptr;
  std::function<void(glm::vec2)> m_onKill;
};

} // namespace platformer
