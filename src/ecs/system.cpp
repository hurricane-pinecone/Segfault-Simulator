#include "system.h"

const std::vector<Entity>& System::getEntities() const { return entities; }

const Signature& System::getComponentSignature() const
{
  return componentSignature;
}

void System::addEntity(Entity entity) { entities.push_back(entity); }

void System::removeEntity(Entity entity)
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
