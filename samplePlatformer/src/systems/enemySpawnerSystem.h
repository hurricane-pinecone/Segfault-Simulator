#pragma once

#include "components/platformerComponents.h"
#include "config.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/system.h"

#include <functional>

namespace platformer
{

/**
 * Spawns enemies on a timer for testing, capped at ENEMY_CAP live enemies. The
 * actual entity creation is delegated to a callback (the scene's createEnemy),
 * so enemy assembly stays in one place; this system only owns the cadence and
 * the live-count cap (which it reads from the registry).
 */
class EnemySpawnerSystem : public sfs::System
{
public:
  void setSpawn(std::function<void()> spawn) { m_spawn = std::move(spawn); }

protected:
  void update(double deltaTime) override
  {
    if (!registry || !m_spawn)
      return;

    m_timer += deltaTime;
    if (m_timer < ENEMY_SPAWN_INTERVAL)
      return;

    m_timer = 0.0;

    if (static_cast<int>(registry->view<Enemy>().size()) < ENEMY_CAP)
      m_spawn();
  }

private:
  std::function<void()> m_spawn;
  double m_timer = 0.0;
};

} // namespace platformer
