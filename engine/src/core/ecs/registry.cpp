#include <engine/core/ecs/entity.h>
#include <engine/core/ecs/registry.h>
#include <engine/core/logger/logger.h>
#include <engine/core/util/fatalError.h>

#include <algorithm>
#include <string>

namespace sfs
{

Registry::~Registry()
{
  // Taken out of the registry first, so a destructor that looks another system
  // up finds none rather than one already destroyed.
  std::vector<std::unique_ptr<System>> leaving = std::move(systems);
  systems.clear();
  forgetSystemLookups();
  for (std::unique_ptr<System>& system : leaving)
    system.reset();
}

void Registry::forgetSystemLookups()
{
  // Counted before the slots are cleared: a lookup storing after the clear
  // then sees the count changed and takes its answer back.
  m_systemsChanged.fetch_add(1);
  for (std::atomic<std::uintptr_t>& slot : m_systemSlots)
    slot.store(kNotLookedUp);
  for (std::atomic<const std::type_info*>& type : m_systemSlotTypes)
    type.store(nullptr, std::memory_order_relaxed);
}

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
    m_links.nodes.resize(entityCount);
  }

  entityAlive[id] = true;
  entityDebugIds[id] = nextDebugId++;
  constexpr Entity::EntityId kNone = HierarchyLinks::kNone;
  m_links.nodes[id] = {kNone, kNone, kNone, kNone};
  ++m_entitySetVersion;

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
  // Observers' signatures are canonical as made; the registered one only
  // now, as registerComponent() must still see a second Any group as one.
  system.m_signature = system.m_signature.canonical();
  const std::size_t lists = system.listCount();
  system.m_lists.clear();
  system.m_lists.reserve(lists);
  for (std::size_t i = 0; i < lists; ++i)
  {
    const uint32_t index = listOf(system.signatureOf(i));
    m_queryFollowed[index] = 1;
    system.m_lists.push_back(&m_queryLists[index]);
  }

  for (Entity::EntityId id = 0; id < entityCount; ++id)
  {
    if (!entityAlive[id] || isPending(id))
      continue;
    const Entity entity = getEntity(id);
    const Signature components = entityComponentSignatures[id];
    for (std::size_t i = 0; i < lists; ++i)
      if (system.signatureOf(i).matches(components))
        system.tellJoined(i, entity);
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
  const uint32_t list = listOf(system.signatureOf(index));
  m_queryFollowed[list] = 1;
  system.m_lists.push_back(&m_queryLists[list]);
  // A callback may change the list.
  const std::vector<Entity> members = m_queryLists[list];
  for (const Entity& entity : members)
    system.tellJoined(index, entity);
}

void Registry::countChange(Entity::EntityId entity,
                           std::size_t componentId,
                           bool gained)
{
  ++m_componentChanges[componentId];
  if (gained)
  {
    if (m_holderCount[componentId]++ == 0)
      m_holder[componentId] = entity;
  }
  else
  {
    if (m_holderCount[componentId] > 0)
      --m_holderCount[componentId];
    if (m_holder[componentId] == entity)
      m_holder[componentId] = kNoHolder;
  }
}

void Registry::countChanges(Entity::EntityId entity,
                            const Signature& before,
                            const Signature& after)
{
  const Signature changed = before ^ after;
#if defined(__GLIBCXX__)
  // Straight from set bit to set bit.
  for (std::size_t id = changed._Find_first(); id < changed.size();
       id = changed._Find_next(id))
    countChange(entity, id, after.test(id));
#else
  // Stops after the last set bit.
  for (std::size_t id = 0, left = changed.count(); left > 0; ++id)
    if (changed.test(id))
    {
      --left;
      countChange(entity, id, after.test(id));
    }
#endif
}

uint32_t Registry::listOf(const QuerySignature& signature)
{
  const auto [found, made] = m_queryIndex.try_emplace(
      signature, static_cast<uint32_t>(m_querySignatures.size()));
  if (!made)
    return found->second;
  const uint32_t index = found->second;
  m_querySignatures.push_back(signature);
  m_queryFollowed.push_back(0);
  std::vector<Entity>& entities = m_queryLists.emplace_back();
  const Signature named = signature.all | signature.any | signature.excluding;
  for (std::size_t componentId = 0; componentId < MAX_COMPONENTS; ++componentId)
    if (named.test(componentId))
      m_queriesNaming[componentId].push_back(index);
  for (Entity::EntityId id = 0; id < entityComponentSignatures.size(); ++id)
    if (entityAlive[id] && !isPending(id) &&
        signature.matches(entityComponentSignatures[id]))
      entities.push_back(getEntity(id));
  return index;
}

void Registry::syncQueries(Entity::EntityId id,
                           const Signature& before,
                           bool wasIn,
                           const Signature& after,
                           bool isIn,
                           const std::vector<uint32_t>* lists)
{
  // Taken, so a hook that changes components anyway gets a scratch of its own.
  std::vector<QueryChange> changes = std::move(m_queryChanges);
  changes.clear();
  bool followed = false;
  const Entity entity = getEntity(id);
  const auto sync = [&](uint32_t index)
  {
    const QuerySignature& signature = m_querySignatures[index];
    const bool was = wasIn && signature.matches(before);
    const bool is = isIn && signature.matches(after);
    if (was == is)
      return;
    // Kept in id order, as a scan would list them.
    std::vector<Entity>& entities = m_queryLists[index];
    auto at = std::lower_bound(entities.begin(),
                               entities.end(),
                               id,
                               [](const Entity& member, Entity::EntityId key)
                               { return member.getId() < key; });
    if (is)
      entities.insert(at, entity);
    else if (at != entities.end() && at->getId() == id)
      entities.erase(at);
    changes.push_back({&entities, is});
    followed = followed || m_queryFollowed[index] != 0;
  };
  if (lists)
    for (const uint32_t index : *lists)
      sync(index);
  else
    for (uint32_t index = 0; index < m_querySignatures.size(); ++index)
      sync(index);

  if (followed)
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

void Registry::componentsChanged(const Entity& entity,
                                 const Signature& before,
                                 std::size_t componentId)
{
  const Entity::EntityId id = entity.getId();
  const Signature& after = entityComponentSignatures[id];
  countChange(id, componentId, after.test(componentId));

  // Not in any query yet: it joins them, as it is then, at the next flush.
  if (isPending(id))
    return;
  syncQueries(id, before, true, after, true, &m_queriesNaming[componentId]);
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

    unlinkRemoved(id);

    for (auto& pool : componentPools)
    {
      if (pool)
      {
        pool->remove(id);
      }
    }

    entityAlive[id] = false;
    generations[id]++;
    ++m_entitySetVersion;

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

void Registry::unlink(Entity::EntityId id)
{
  constexpr Entity::EntityId kNone = HierarchyLinks::kNone;
  std::vector<HierarchyNode>& nodes = m_links.nodes;
  HierarchyNode& node = nodes[id];
  if (node.parent == kNone)
    return;
  HierarchyNode& parent = nodes[node.parent];
  if (parent.firstChild == id)
  {
    // The first child's prevSibling is the last child, which stays so.
    parent.firstChild = node.nextSibling;
    if (node.nextSibling != kNone)
      nodes[node.nextSibling].prevSibling = node.prevSibling;
  }
  else
  {
    nodes[node.prevSibling].nextSibling = node.nextSibling;
    if (node.nextSibling != kNone)
      nodes[node.nextSibling].prevSibling = node.prevSibling;
    else
      nodes[parent.firstChild].prevSibling = node.prevSibling;
  }
  node.parent = kNone;
  node.nextSibling = kNone;
  node.prevSibling = kNone;
}

void Registry::relink(Entity::EntityId child, Entity::EntityId parent)
{
  constexpr Entity::EntityId kNone = HierarchyLinks::kNone;
  std::vector<HierarchyNode>& nodes = m_links.nodes;
  if (nodes[child].parent == parent)
    return;
  unlink(child);
  ++m_links.linkVersion;
  if (parent == kNone)
    return;

  HierarchyNode& node = nodes[child];
  HierarchyNode& above = nodes[parent];
  node.parent = parent;
  if (above.firstChild == kNone)
  {
    above.firstChild = child;
    node.prevSibling = child;
    return;
  }
  const Entity::EntityId first = above.firstChild;
  const Entity::EntityId last = nodes[first].prevSibling;
  if (child < first)
  {
    node.nextSibling = first;
    node.prevSibling = last;
    nodes[first].prevSibling = child;
    above.firstChild = child;
    return;
  }
  // Loads and spawns make children in rising id order, so the tail is
  // nearly always the place; a reused lower id walks back to its own.
  Entity::EntityId before = last;
  while (before > child)
    before = nodes[before].prevSibling;
  node.prevSibling = before;
  node.nextSibling = nodes[before].nextSibling;
  if (node.nextSibling != kNone)
    nodes[node.nextSibling].prevSibling = child;
  else
    nodes[first].prevSibling = child;
  nodes[before].nextSibling = child;
}

void Registry::unlinkRemoved(Entity::EntityId id)
{
  constexpr Entity::EntityId kNone = HierarchyLinks::kNone;
  std::vector<HierarchyNode>& nodes = m_links.nodes;
  HierarchyNode& node = nodes[id];
  if (node.parent == kNone && node.firstChild == kNone)
    return;
  unlink(id);
  for (Entity::EntityId child = node.firstChild; child != kNone;)
  {
    HierarchyNode& orphan = nodes[child];
    child = orphan.nextSibling;
    orphan.parent = kNone;
    orphan.nextSibling = kNone;
    orphan.prevSibling = kNone;
  }
  node.firstChild = kNone;
  ++m_links.linkVersion;
}

const std::vector<HierarchyLinks::Placed>& Registry::preorder()
{
  constexpr Entity::EntityId kNone = HierarchyLinks::kNone;
  const uint64_t now = m_links.linkVersion + m_entitySetVersion;
  std::vector<HierarchyLinks::Placed>& out = m_links.preorder;
  if (m_links.preorderBuiltAt == now)
    return out;
  m_links.preorderBuiltAt = now;
  out.clear();
  out.reserve(entityCount);
  const std::vector<HierarchyNode>& nodes = m_links.nodes;
  for (Entity::EntityId root = 0; root < entityCount; ++root)
  {
    if (!entityAlive[root] || nodes[root].parent != kNone)
      continue;
    // Down to the first child, else across to the next sibling of the
    // nearest entity on the way back up that has one.
    out.push_back({root, kNone});
    Entity::EntityId at = root;
    uint32_t atPos = static_cast<uint32_t>(out.size() - 1);
    while (true)
    {
      if (const Entity::EntityId child = nodes[at].firstChild; child != kNone)
      {
        out.push_back({child, atPos});
        at = child;
        atPos = static_cast<uint32_t>(out.size() - 1);
        continue;
      }
      while (at != root && nodes[at].nextSibling == kNone)
      {
        atPos = out[atPos].parentPos;
        at = out[atPos].id;
      }
      if (at == root)
        break;
      at = nodes[at].nextSibling;
      out.push_back({at, out[atPos].parentPos});
      atPos = static_cast<uint32_t>(out.size() - 1);
    }
  }
  return out;
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
