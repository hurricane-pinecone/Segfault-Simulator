#include <engine/ecs/system.h>

namespace sfs
{

const std::vector<Entity>& System::getEntities() const { return entities; }

const Signature& System::getComponentSignature() const
{
  return componentSignature;
}

void System::addEntity(const Entity& entity) { entities.emplace_back(entity); }

void System::removeEntity(const Entity& entity)
{
  for (size_t i = 0; i < entities.size(); i++)
  {
    if (entities[i] == entity)
    {
      entities[i] = entities.back();
      entities.pop_back();
      return;
    }
  }
}

} // namespace sfs
