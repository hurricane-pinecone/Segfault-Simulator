#include <engine/core/ecs/system.h>

namespace sfs
{

const std::vector<Entity>& System::getEntities() const
{
  return m_sets.front().entities;
}
std::vector<Entity>& System::getEntities() { return m_sets.front().entities; }

const std::vector<Entity>& System::getEntities(int set) const
{
  static const std::vector<Entity> none;
  return set >= 0 && static_cast<std::size_t>(set) < m_sets.size()
             ? m_sets[static_cast<std::size_t>(set)].entities
             : none;
}

const Signature& System::getComponentSignature() const
{
  return m_sets.front().signature;
}

void System::sync(const Entity& entity, const Signature& components, bool alive)
{
  for (std::size_t i = 0; i < m_sets.size(); ++i)
  {
    Set& set = m_sets[i];
    const bool belongs = alive && (components & set.signature) == set.signature;
    if (belongs == contains(set, entity))
      continue;
    if (belongs)
    {
      add(set, entity);
      onEntityAdded(entity, static_cast<int>(i));
    }
    else
    {
      remove(set, entity);
      onEntityRemoved(entity, static_cast<int>(i));
    }
  }
}

bool System::contains(const Set& set, const Entity& entity) const
{
  const Entity::EntityId id = entity.getId();
  return id < set.positions.size() && set.positions[id] != kNowhere;
}

void System::add(Set& set, const Entity& entity)
{
  const Entity::EntityId id = entity.getId();
  if (id >= set.positions.size())
    set.positions.resize(id + 1, kNowhere);
  set.positions[id] = static_cast<uint32_t>(set.entities.size());
  set.entities.emplace_back(entity);
}

void System::remove(Set& set, const Entity& entity)
{
  // Where it is, by id (a game may have reordered the list: then look).
  const Entity::EntityId id = entity.getId();
  std::size_t at = id < set.positions.size() ? set.positions[id] : kNowhere;
  if (at >= set.entities.size() || set.entities[at] != entity)
  {
    at = kNowhere;
    for (std::size_t i = 0; i < set.entities.size(); i++)
      if (set.entities[i] == entity)
      {
        at = i;
        break;
      }
  }
  if (id < set.positions.size())
    set.positions[id] = kNowhere;
  if (at == kNowhere)
    return;

  if (at + 1 != set.entities.size())
  {
    set.entities[at] = set.entities.back();
    const Entity::EntityId moved = set.entities[at].getId();
    if (moved < set.positions.size())
      set.positions[moved] = static_cast<uint32_t>(at);
  }
  set.entities.pop_back();
}

} // namespace sfs
