#include "entity.h"

int Entity::getId() const { return id; }

void Entity::setRegistry(Registry* registry) { this->registry = registry; }
