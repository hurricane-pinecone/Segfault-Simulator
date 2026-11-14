#include "engine/runtime/systems/transformSystem.h"

#include "engine/core/components/transformComponent.h"
#include "engine/core/components/worldTransformComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/util/debugFeatures.h"

namespace sfs
{

void TransformSystem::create() { registerComponent<TransformComponent>(); }

void TransformSystem::onEntityAdded(const Entity& entity)
{
  if (!entity.hasComponent<WorldTransformComponent>())
    Entity(entity).addComponent<WorldTransformComponent>();
}

void TransformSystem::onEntityRemoved(const Entity& entity)
{
  // A destroyed entity still has its transform here, and the flush removes
  // the cache with the rest.
  if (!entity.hasComponent<TransformComponent>())
    entity.removeComponent<WorldTransformComponent>();
}

void TransformSystem::render()
{
  constexpr Entity::EntityId kNone = HierarchyLinks::kNone;
  const std::vector<HierarchyLinks::Placed>& order =
      hierarchy::preorder(*registry);
  m_above.resize(order.size());
  const uint32_t pass = m_pass;
  // The preorder lists living entities only.
  const auto transforms = registry->finder<TransformComponent>();
  const auto caches = registry->finder<WorldTransformComponent>();
  for (std::size_t pos = 0; pos < order.size(); ++pos)
  {
    const HierarchyLinks::Placed& placed = order[pos];
    const Above above =
        placed.parentPos == kNone ? Above{} : m_above[placed.parentPos];
    const TransformComponent* local = transforms(placed.id);
    if (!local)
    {
      m_above[pos] = above; // it only groups
      continue;
    }
    // Under an entity with no cache yet, or without one itself: not made
    // until both have one, so it's read fresh.
    WorldTransformComponent* cache = caches(placed.id);
    if (!cache || (!above.cache && above.id != kNone))
    {
      if (cache)
        cache->version = 0;
      m_above[pos] = {nullptr, placed.id};
      continue;
    }

    const uint64_t hash = hierarchy::transformHash(*local);
    if (!hierarchy::cacheCurrent(*cache, hash, above.id) ||
        (above.cache && above.cache->version == pass))
    {
      cache->world = hierarchy::composeWorld(
          above.cache ? &above.cache->world : nullptr, *local);
      cache->localHash = hash;
      cache->parent = above.id;
      cache->version = pass;
    }
    m_above[pos] = {cache, placed.id};
  }
  if (++m_pass == 0)
    m_pass = 1;

  if (debugFeatures::checkTransforms())
    checkCaches();
}

void TransformSystem::checkCaches()
{
  const auto caches = registry->finder<WorldTransformComponent>();
  for (const Entity& entity : getEntities())
    if (const WorldTransformComponent* cache = caches(entity.getId());
        cache && cache->version != 0)
      hierarchy::checkWorldCache(entity, *cache, "after the transform pass");
}

} // namespace sfs
