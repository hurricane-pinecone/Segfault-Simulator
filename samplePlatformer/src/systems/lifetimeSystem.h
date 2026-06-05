#pragma once

#include "components/platformerComponents.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/system.h"

#include <vector>

namespace platformer
{

// Counts down each Lifetime and destroys the entity when it expires. Used for
// short-lived effect entities (muzzle / death flash lights).
class LifetimeSystem : public sfs::System
{
protected:
  void create() override { registerComponent<Lifetime>(); }

  void update(double deltaTime) override
  {
    if (!registry)
      return;

    std::vector<sfs::Entity> dead;
    for (const auto& entity : getEntities())
    {
      auto& lifetime = entity.getComponent<Lifetime>();
      lifetime.remaining -= static_cast<float>(deltaTime);
      if (lifetime.remaining <= 0.0f)
        dead.push_back(entity);
    }

    for (const auto& entity : dead)
      registry->destroyEntity(entity);
  }
};

} // namespace platformer
