#pragma once

#include "SDL_pixels.h"
#include "components/platformerComponents.h"
#include "config.h"
#include "engine/core/components/boxCollider2D.h"
#include "engine/core/components/lightEmitterComponent.h"
#include "engine/core/components/rigidBodyComponent.h"
#include "engine/core/components/tags/solidObject.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/system.h"
#include "engine/core/particles/particleEngine.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"
#include "systems/bloodSystem.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <random>
#include <vector>

namespace platformer
{

/**
 * Advances bullets and applies their stacked spell modifiers: Homing steers
 * toward the nearest enemy, Gravity arcs them, Bounce ricochets off platforms,
 * Pierce passes through enemies, Explosive detonates (area damage + flash) on
 * impact, and Chain arcs lightning to nearby enemies. Hits spray blood; kills
 * fire a gib burst + the onKill callback (score / shake / spell drop).
 */
class BulletSystem : public sfs::System
{
public:
  // The particle engine effects fire through (the FlatRenderSystem's Particles
  // module): "blood" / "gore" / "explosion" / "spark".
  void setParticles(sfs::ParticleEngine* particles) { m_particles = particles; }

  // Called at an enemy's death position (score, screen shake, spell drop).
  void setOnKill(std::function<void(glm::vec2)> onKill)
  {
    m_onKill = std::move(onKill);
  }

  // Physics blood: hits/kills spray sticking droplets through this system.
  void setBlood(BloodSystem* blood) { m_blood = blood; }

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

    for (const auto& bulletEntity : getEntities())
    {
      auto& transform = bulletEntity.getComponent<sfs::TransformComponent>();
      auto& bullet = bulletEntity.getComponent<Bullet>();

      if (bullet.homing)
        steerToward(transform.position, bullet, dt);

      bullet.velocity.y += bullet.gravity * dt;
      transform.position += bullet.velocity * dt;

      if (bullet.bounces > 0)
        bounceOffPlatforms(transform.position, bulletHalf, bullet);

      transform.rotation = glm::atan(bullet.velocity.y, bullet.velocity.x);

      bullet.life -= dt;
      if (bullet.life <= 0.0f)
      {
        deadBullets.push_back(bulletEntity);
        continue;
      }

      bool consumed = false;
      for (const auto& enemyEntity :
           registry->view<Enemy, sfs::TransformComponent, sfs::BoxCollider2D>())
      {
        auto& enemy = enemyEntity.getComponent<Enemy>();
        if (enemy.health <= 0.0f)
          continue;

        const glm::vec2 enemyPos =
            enemyEntity.getComponent<sfs::TransformComponent>().position;
        const glm::vec2 enemyHalf =
            enemyEntity.getComponent<sfs::BoxCollider2D>().half;
        if (!overlaps(transform.position, bulletHalf, enemyPos, enemyHalf))
          continue;

        enemyEntity.getComponent<sfs::RigidBodyComponent>().velocity.y =
            -ENEMY_KNOCKBACK;
        damageEnemy(enemyEntity, bullet.damage, transform.position);

        // Spray blood along the bullet's travel; the droplets fly and stick to
        // whatever platform they land on (BloodSystem), so the stains trace the
        // actual impact, not a drawn shape.
        if (m_blood)
          m_blood->spray(transform.position,
                         bullet.velocity,
                         12,
                         120.0f,
                         520.0f,
                         60.0f,
                         SDL_Color{200, 16, 16, 255},
                         10.0f);

        if (bullet.chain)
          chainArc(enemyPos, bullet.color, enemyEntity);

        if (bullet.explosive)
        {
          explode(transform.position, bullet.color);
          consumed = true;
          break;
        }
        if (!bullet.pierce)
        {
          consumed = true;
          break;
        }
      }

      if (consumed)
        deadBullets.push_back(bulletEntity);
    }

    for (const auto& bullet : deadBullets)
      registry->destroyEntity(bullet);
    for (const auto& enemy : m_deadEnemies)
      registry->destroyEntity(enemy);
    m_deadEnemies.clear();
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

  bool nearestEnemyPos(const glm::vec2& from,
                       glm::vec2& out,
                       const sfs::Entity& exclude)
  {
    float best = std::numeric_limits<float>::max();
    bool found = false;
    for (const auto& e :
         registry->view<Enemy, sfs::TransformComponent, sfs::BoxCollider2D>())
    {
      if (e == exclude || e.getComponent<Enemy>().health <= 0.0f)
        continue;
      const glm::vec2 p = e.getComponent<sfs::TransformComponent>().position;
      const float d2 =
          (p.x - from.x) * (p.x - from.x) + (p.y - from.y) * (p.y - from.y);
      if (d2 < best)
      {
        best = d2;
        out = p;
        found = true;
      }
    }
    return found;
  }

  void steerToward(const glm::vec2& pos, Bullet& bullet, float dt)
  {
    glm::vec2 target;
    if (!nearestEnemyPos(pos, target, sfs::Entity{}))
      return;

    const float speed = glm::length(bullet.velocity);
    if (speed < 1.0f)
      return;

    const glm::vec2 cur = bullet.velocity / speed;
    const glm::vec2 desired = glm::normalize(target - pos);
    glm::vec2 next = cur + (desired - cur) * glm::min(1.0f, HOMING_TURN * dt);
    const float len = glm::length(next);
    if (len > 0.001f)
      bullet.velocity = (next / len) * speed;
  }

  void bounceOffPlatforms(glm::vec2& pos, const glm::vec2& half, Bullet& bullet)
  {
    for (const auto& solid : registry->view<sfs::SolidObject,
                                            sfs::TransformComponent,
                                            sfs::BoxCollider2D>())
    {
      const glm::vec2 sc =
          solid.getComponent<sfs::TransformComponent>().position;
      const glm::vec2 sh = solid.getComponent<sfs::BoxCollider2D>().half;
      if (!overlaps(pos, half, sc, sh))
        continue;

      // Reflect off whichever face is shallowest (the one being crossed).
      const float ox = (sh.x + half.x) - glm::abs(pos.x - sc.x);
      const float oy = (sh.y + half.y) - glm::abs(pos.y - sc.y);
      if (ox < oy)
      {
        bullet.velocity.x = -bullet.velocity.x;
        pos.x += pos.x < sc.x ? -ox : ox;
      }
      else
      {
        bullet.velocity.y = -bullet.velocity.y;
        pos.y += pos.y < sc.y ? -oy : oy;
      }
      --bullet.bounces;
      if (m_particles)
        m_particles->spawnBurst("spark", pos, 0.0f);
      break;
    }
  }

  void
  damageEnemy(const sfs::Entity& entity, float amount, const glm::vec2& hitPos)
  {
    auto& enemy = entity.getComponent<Enemy>();
    if (enemy.health <= 0.0f)
      return;

    enemy.health -= amount;
    if (m_particles)
      m_particles->spawnBurst("blood", hitPos, 0.0f);

    if (enemy.health <= 0.0f)
    {
      const glm::vec2 pos =
          entity.getComponent<sfs::TransformComponent>().position;
      if (m_particles)
        m_particles->spawnBurst("gore", pos, 0.0f);
      // A heavy radial eruption of sticking droplets -- they fly up and out,
      // then gravity rains them back onto the platforms as organic splatter.
      if (m_blood)
        m_blood->spray(pos,
                       glm::vec2{0.0f, 0.0f},
                       46,
                       140.0f,
                       680.0f,
                       260.0f,
                       SDL_Color{180, 12, 12, 255},
                       13.0f);
      if (m_onKill)
        m_onKill(pos);
      m_deadEnemies.push_back(entity);
    }
  }

  void explode(const glm::vec2& center, const glm::vec3& color)
  {
    if (m_particles)
      m_particles->spawnBurst("explosion", center, 0.0f);
    spawnFlash(
        center, glm::vec3{1.0f, 0.6f, 0.25f}, EXPLOSION_RADIUS * 2.0f, 0.22f);

    for (const auto& e :
         registry->view<Enemy, sfs::TransformComponent, sfs::BoxCollider2D>())
    {
      if (e.getComponent<Enemy>().health <= 0.0f)
        continue;
      const glm::vec2 p = e.getComponent<sfs::TransformComponent>().position;
      const glm::vec2 d = p - center;
      const float dist = glm::length(d);
      if (dist >= EXPLOSION_RADIUS)
        continue;

      const glm::vec2 dir = dist > 0.01f ? d / dist : glm::vec2{0.0f, -1.0f};
      auto& rb = e.getComponent<sfs::RigidBodyComponent>();
      rb.velocity += dir * 260.0f;
      rb.velocity.y -= 220.0f;
      damageEnemy(e, EXPLOSION_DAMAGE, p);
    }
    (void)color;
  }

  void chainArc(const glm::vec2& from,
                const glm::vec3& color,
                const sfs::Entity& source)
  {
    struct Target
    {
      sfs::Entity entity;
      glm::vec2 pos;
      float d2;
    };
    std::vector<Target> targets;
    for (const auto& e :
         registry->view<Enemy, sfs::TransformComponent, sfs::BoxCollider2D>())
    {
      if (e == source || e.getComponent<Enemy>().health <= 0.0f)
        continue;
      const glm::vec2 p = e.getComponent<sfs::TransformComponent>().position;
      const float d2 =
          (p.x - from.x) * (p.x - from.x) + (p.y - from.y) * (p.y - from.y);
      if (d2 < CHAIN_RANGE * CHAIN_RANGE)
        targets.push_back({e, p, d2});
    }
    std::sort(targets.begin(),
              targets.end(),
              [](const Target& a, const Target& b) { return a.d2 < b.d2; });

    const int count = glm::min(CHAIN_TARGETS, static_cast<int>(targets.size()));
    for (int i = 0; i < count; ++i)
    {
      if (m_particles)
        m_particles->spawnBurst("spark", targets[i].pos, 0.0f);
      spawnFlash(targets[i].pos, color, 140.0f, 0.12f);
      damageEnemy(targets[i].entity, CHAIN_DAMAGE, targets[i].pos);
    }
  }

  void spawnFlash(const glm::vec2& pos,
                  const glm::vec3& color,
                  float radius,
                  float time)
  {
    registry->createEntity()
        .addComponent<sfs::TransformComponent>(pos)
        .addComponent<sfs::LightEmitterComponent>(radius, 3.0f, 0.0f, color)
        .addComponent<Lifetime>(time);
  }

  sfs::ParticleEngine* m_particles = nullptr;
  BloodSystem* m_blood = nullptr;
  std::function<void(glm::vec2)> m_onKill;
  std::vector<sfs::Entity> m_deadEnemies;
  std::mt19937 m_rng{99};
};

} // namespace platformer
