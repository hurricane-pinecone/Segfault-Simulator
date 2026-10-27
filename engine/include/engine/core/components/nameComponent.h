#pragma once

#include <string>
#include <utility>

namespace sfs
{

/**
 * A human-readable name for an entity, shown in an editor's hierarchy and
 * usable for lookups in game code. Optional: an unnamed entity is identified by
 * its id.
 *
 * @param std::string name - display name
 */
struct NameComponent
{
  std::string name;

  NameComponent() = default;
  explicit NameComponent(std::string name) : name(std::move(name)) {}
};

} // namespace sfs
