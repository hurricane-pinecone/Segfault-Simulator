#include <engine/core/ecs/entity.h>
#include <engine/core/ecs/registry.h>
#include <engine/core/logger/logger.h>
#include <engine/core/util/fatalError.h>

#include <algorithm>
#include <string>

namespace sfs
{

Entity Registry::createEntity()
{
  Entity::EntityId id;

  if (!freeEntityIds.empty())
  {
    id = *freeEntityIds.begin();
    freeEntityIds.erase(freeEntityIds.begin());
  }
  else
  {
    id = entityCount++;

    entityComponentSignatures.resize(entityCount);
    generations.resize(entityCount, 0);
    entityDebugIds.resize(entityCount, 0);
    entityAlive.resize(entityCount, false);
  }

  entityAlive[id] = true;
  entityDebugIds[id] = nextDebugId++;

  Entity entity{id, generations[id], entityDebugIds[id]};
  entity.setRegistry(this);

  entitiesToBeAdded.insert(entity);

  return entity;
}

Entity Registry::getEntity(Entity::EntityId id)
{
  if (id >= entityCount)
    return {};

  if (!entityAlive[id])
    return {};

  Entity entity{id, generations[id], entityDebugIds[id]};
  entity.setRegistry(this);

  return entity;
}

bool Registry::isPending(Entity::EntityId id) const
{
  if (entitiesToBeAdded.empty())
    return false;
  const Entity probe{id, generations[id], entityDebugIds[id]};
  return entitiesToBeAdded.contains(probe);
}

void Registry::attach(System& system)
{
  const std::size_t lists = system.listCount();
  system.m_lists.clear();
  system.m_lists.reserve(lists);
  for (std::size_t i = 0; i < lists; ++i)
    system.m_lists.push_back(&queryOf(system.signatureOf(i)));

  for (Entity::EntityId id = 0; id < entityCount; ++id)
  {
    if (!entityAlive[id] || isPending(id))
      continue;
    const Entity entity = getEntity(id);
    const Signature components = entityComponentSignatures[id];
    for (std::size_t i = 0; i < lists; ++i)
    {
      const Signature& signature = system.signatureOf(i);
      if ((components & signature) == signature)
        system.tellJoined(i, entity);
    }
  }
}

void Registry::follow(System& system, std::size_t index)
{
  if (index != system.m_lists.size() || index >= system.listCount())
  {
    LOG_ERROR("Registry::follow: list " + std::to_string(index) +
              " is out of order; not followed");
    return;
  }
  const std::vector<Entity>& list = queryOf(system.signatureOf(index));
  system.m_lists.push_back(&list);
  const std::vector<Entity> members = list; // a callback may change the list
  for (const Entity& entity : members)
    system.tellJoined(index, entity);
}

void Registry::countChanges(Entity::EntityId entity,
                            const Signature& before,
                            const Signature& after)
{
  const Signature changed = before ^ after;
  if (changed.none())
    return;
  for (std::size_t id = 0; id < m_componentChanges.size(); ++id)
  {
    if (!changed.test(id))
      continue;
    ++m_componentChanges[id];
    if (after.test(id))
    {
      if (m_holderCount[id]++ == 0)
        m_holder[id] = entity;
    }
    else
    {
      if (m_holderCount[id] > 0)
        --m_holderCount[id];
      if (m_holder[id] == entity)
        m_holder[id] = kNoHolder;
    }
  }
}

const std::vector<Entity>& Registry::queryOf(const Signature& signature)
{
  auto found = m_queries.find(signature);
  if (found != m_queries.end())
    return found->second.entities;
  Query& query = m_queries[signature];
  query.signature = signature;
  for (Entity::EntityId id = 0; id < entityComponentSignatures.size(); ++id)
    if (entityAlive[id] && !isPending(id) &&
        (entityComponentSignatures[id] & signature) == signature)
      query.entities.push_back(getEntity(id));
  return query.entities;
}

void Registry::syncQueries(Entity::EntityId id,
                           const Signature& before,
                           bool wasIn,
                           const Signature& after,
                           bool isIn)
{
  // Taken, so a hook that changes components anyway gets a scratch of its own.
  std::vector<QueryChange> changes = std::move(m_queryChanges);
  changes.clear();
  const Entity entity = getEntity(id);
  for (auto& [signature, query] : m_queries)
  {
    const bool was = wasIn && (before & signature) == signature;
    const bool is = isIn && (after & signature) == signature;
    if (was == is)
      continue;
    // Kept in id order, as a scan would list them.
    auto at = std::lower_bound(query.entities.begin(),
                               query.entities.end(),
                               id,
                               [](const Entity& member, Entity::EntityId key)
                               { return member.getId() < key; });
    if (is)
      query.entities.insert(at, entity);
    else if (at != query.entities.end() && at->getId() == id)
      query.entities.erase(at);
    changes.push_back({&query.entities, is});
  }

  if (!changes.empty())
    for (std::size_t s = 0; s < systems.size(); ++s)
    {
      System& system = *systems[s];
      for (std::size_t i = 0; i < system.m_lists.size(); ++i)
        for (const QueryChange& change : changes)
        {
          if (change.list != system.m_lists[i])
            continue;
          if (change.joined)
            system.tellJoined(i, entity);
          else
            system.tellLeft(i, entity);
          break;
        }
    }
  m_queryChanges = std::move(changes);
}

Entity::EntityId Registry::findHolder(std::size_t componentId) const
{
  for (Entity::EntityId id = 0; id < entityComponentSignatures.size(); ++id)
    if (entityAlive[id] && entityComponentSignatures[id].test(componentId))
      return id;
  return kNoHolder;
}

void Registry::warnNotSingle(std::size_t componentId, std::string_view type)
{
  m_warnedNotSingle[componentId] = true;
  LOG_WARN("singleton(): " + std::to_string(m_holderCount[componentId]) +
           " entities have a " + std::string(type) + "; using one of them");
}

void Registry::componentsChanged(const Entity& entity, const Signature& before)
{
  const Entity::EntityId id = entity.getId();
  const Signature& after = entityComponentSignatures[id];
  countChanges(id, before, after);

  // Not in any query yet: it joins them, as it is then, at the next flush.
  if (isPending(id))
    return;
  syncQueries(id, before, true, after, true);
}

void Registry::badComponentAccess(const Entity& entity,
                                  const char* action,
                                  std::string_view type) const
{
  const std::string what = std::string(action) + "<" +
                           detail::readableTypeName(type) + ">: entity " +
                           std::to_string(entity.getId());
  const auto id = entity.getId();
  if (!entity.isValid())
    fatalError(what + " isn't an entity (a default-made Entity)");
  if (id >= entityAlive.size() || !entityAlive[id])
    fatalError(what + " was destroyed");
  if (generations[id] != entity.getGeneration())
    fatalError(what + " was destroyed (its id now belongs to another entity)");
  fatalError(what + " has no such component");
}

namespace detail
{
void invalidEntity(const char* action)
{
  fatalError(std::string(action) +
             " on an Entity that isn't one (default-made, or reset)");
}
} // namespace detail

void Registry::flushEntities()
{
  for (const auto& entity : entitiesToBeRemoved)
  {
    if (!isAlive(entity))
      continue;

    const auto id = entity.getId();

    // Still pending its first flush: in no query yet.
    if (!entitiesToBeAdded.contains(entity))
      syncQueries(id,
                  entityComponentSignatures[id],
                  true,
                  entityComponentSignatures[id],
                  false);

    if (id < entityComponentSignatures.size())
    {
      countChanges(id, entityComponentSignatures[id], Signature());
      entityComponentSignatures[id].reset();
    }

    for (auto& pool : componentPools)
    {
      if (pool)
      {
        pool->remove(id);
      }
    }

    entityAlive[id] = false;
    generations[id]++;

    freeEntityIds.insert(id);
  }

  entitiesToBeRemoved.clear();

  // One at a time, in id order: the others stay pending (their component
  // changes wait) while one joins and its hooks run.
  while (!entitiesToBeAdded.empty())
  {
    const Entity entity = *entitiesToBeAdded.begin();
    entitiesToBeAdded.erase(entitiesToBeAdded.begin());
    if (!isAlive(entity))
      continue;
    const Signature& components = entityComponentSignatures[entity.getId()];
    syncQueries(entity.getId(), components, false, components, true);
  }
}

void Registry::destroyEntity(const Entity& entity)
{
  if (!isAlive(entity))
    return;

  if (entitiesToBeRemoved.contains(entity))
    return;

  entitiesToBeRemoved.insert(entity);
}

std::vector<Entity> Registry::entities()
{
  std::vector<Entity> result;
  for (Entity::EntityId id = 0; id < entityCount; ++id)
    if (entityAlive[id])
      result.push_back(getEntity(id));
  return result;
}

bool Registry::isAlive(const Entity& entity) const
{
  const auto id = entity.getId();

  return id < entityAlive.size() && entityAlive[id] &&
         generations[id] == entity.getGeneration();
}

} // namespace sfs
