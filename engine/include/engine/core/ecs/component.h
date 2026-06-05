#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/logger/logger.h"
#include <string>

namespace sfs
{

struct BaseComponent
{
protected:
  inline static int nextId = 0;
};

template <typename T>
class Component : public BaseComponent
{
public:
  // Unique id of component T
  // IE: entity1 TransformComponent::getId() -> 1
  //     entity2 TransformComponent::getid() -> 1
  static int getId()
  {
    static int id = nextId++;

    // The id is a bit index into a fixed-width Signature; an id at or past the
    // ceiling has no bit to represent it. Report it here, where the type is
    // known, rather than as an opaque out-of-range thrown deep in the registry.
    if (id >= static_cast<int>(MAX_COMPONENTS))
      LOG_ERROR("Component type id " + std::to_string(id) +
                " exceeds MAX_COMPONENTS (" + std::to_string(MAX_COMPONENTS) +
                "); raise MAX_COMPONENTS in entity.h");

    return id;
  }
};

} // namespace sfs
