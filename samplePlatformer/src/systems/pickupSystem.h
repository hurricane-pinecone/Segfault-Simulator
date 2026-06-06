#pragma once

#include "components/platformerComponents.h"
#include "config.h"
#include "spells.h"
#include "engine/core/components/lightEmitterComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/system.h"
#include "engine/core/particles/particleEngine.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"

#include <vector>

namespace platformer
{

/**
 * Collects spell-drop orbs: when the player touches one, its spell is appended
 * to the player's Loadout (so every future shot gains the modifier), a pickup
 * burst + flash fires, and the orb is destroyed. Also pulses each orb's light
 * so it reads as a live, collectable pickup.
 */
class PickupSystem : public sfs::System
{
public:
  void setParticles(sfs::ParticleEngine* particles) { m_particles = particles; }

protected:
  void create() override
  {
    registerComponent<SpellPickup>();
    registerComponent<sfs::TransformComponent>();
  }

  void update(double deltaTime) override
  {
    if (!registry)
      return;

    const float dt = static_cast<float>(deltaTime);

    glm::vec2 playerPos{0.0f, 0.0f};
    sfs::Entity playerEntity;
    bool havePlayer = false;
    for (const auto& p :
         registry->view<PlayerTag, sfs::TransformComponent, Loadout>())
    {
      playerPos = p.getComponent<sfs::TransformComponent>().position;
      playerEntity = p;
      havePlayer = true;
      break;
    }

    std::vector<sfs::Entity> collected;
    for (const auto& entity : getEntities())
    {
      const glm::vec2 pos =
          entity.getComponent<sfs::TransformComponent>().position;
      auto& pickup = entity.getComponent<SpellPickup>();

      // Pulse the orb's glow.
      pickup.bob += dt;
      if (entity.hasComponent<sfs::LightEmitterComponent>())
        entity.getComponent<sfs::LightEmitterComponent>().intensity =
            1.7f + 0.7f * glm::sin(pickup.bob * 6.0f);

      if (!havePlayer)
        continue;

      if (glm::length(pos - playerPos) < PICKUP_RADIUS)
      {
        playerEntity.getComponent<Loadout>().spells.push_back(pickup.spell);
        if (m_particles)
          m_particles->spawnBurst("pickup", pos, 0.0f);
        spawnFlash(pos, spellColor(pickup.spell), 420.0f, 0.3f);
        collected.push_back(entity);
      }
    }

    for (const auto& entity : collected)
      registry->destroyEntity(entity);
  }

private:
  void spawnFlash(const glm::vec2& pos, const glm::vec3& color, float radius,
                  float time)
  {
    registry->createEntity()
        .addComponent<sfs::TransformComponent>(pos)
        .addComponent<sfs::LightEmitterComponent>(radius, 3.2f, 0.0f, color)
        .addComponent<Lifetime>(time);
  }

  sfs::ParticleEngine* m_particles = nullptr;
};

} // namespace platformer
