#pragma once

#include "engine/core/components/nameComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)

#include <string>

/**
 * An entity as the editor names it everywhere (the Hierarchy, the Inspector,
 * reference slots, the viewport).
 *
 * @param entity the entity
 * @return its name, or "Entity <id>" when it has none (or an empty one)
 */
inline std::string entityLabel(const sfs::Entity& entity)
{
  if (const sfs::NameComponent* named =
          entity.getComponent<sfs::NameComponent>();
      named && !named->name.empty())
    return named->name;
  return "Entity " + std::to_string(entity.getId());
}
