#include <engine/core/ecs/system.h>

namespace sfs
{

const std::vector<Entity>& System::getEntities() const { return entities; }
std::vector<Entity>& System::getEntities() { return entities; }

const Signature& System::getComponentSignature() const
{
  return componentSignature;
}

void System::addEntity(const Entity& entity)
{
  const Entity::EntityId id = entity.getId();
  if (id >= m_positions.size())
    m_positions.resize(id + 1, kNowhere);
  m_positions[id] = static_cast<uint32_t>(entities.size());
  entities.emplace_back(entity);
}

void System::removeEntity(const Entity& entity)
{
  // Where it is, by id (a game may have reordered the list: then look).
  const Entity::EntityId id = entity.getId();
  std::size_t at = id < m_positions.size() ? m_positions[id] : kNowhere;
  if (at >= entities.size() || entities[at] != entity)
  {
    at = kNowhere;
    for (std::size_t i = 0; i < entities.size(); i++)
      if (entities[i] == entity)
      {
        at = i;
        break;
      }
    if (at == kNowhere)
      return;
  }

  if (at + 1 != entities.size())
  {
    entities[at] = entities.back();
    const Entity::EntityId moved = entities[at].getId();
    if (moved < m_positions.size())
      m_positions[moved] = static_cast<uint32_t>(at);
  }
  entities.pop_back();
  if (id < m_positions.size())
    m_positions[id] = kNowhere;
}

} // namespace sfs
