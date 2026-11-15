#pragma once

#include "engine/core/ecs/entity.h"

#include <cstddef>
#include <functional>
#include <string>

namespace sfs::reflect
{
struct TypeInfo;
} // namespace sfs::reflect

// Filling [[sfs::part("name")]] reference fields from the names of the model
// nodes below their entity, as the editor does it: the Inspector's Fill Parts,
// a component just added, a prefab opened or applied (refilling what a
// renamed node left stale), and `SfsEditor --fill-parts`. Never at runtime:
// what's filled is saved, and loads as an authored reference.
namespace parts
{

/**
 * Whether a component type has [[sfs::part]] fields.
 *
 * @param type the component's type
 * @return true if any Entity or EntityList field names a part
 */
bool hasParts(const sfs::reflect::TypeInfo& type);

/**
 * Fill a component's [[sfs::part]] fields from the names of its entity's
 * descendants, in preorder, a name matching as sfs::matchesPart() says: an
 * Entity field takes the first match (logged when there are several), an
 * EntityList every match, in order. A field with no match keeps its value
 * (logged). Each field filled is logged.
 *
 * @param entity    the entity with the component
 * @param type      the component's type
 * @param onlyEmpty fill only the fields that are empty (a component just
 *                  added), not ones already set
 * @return how many fields changed
 */
std::size_t fill(const sfs::Entity& entity,
                 const sfs::reflect::TypeInfo& type,
                 bool onlyEmpty = false);

/**
 * fill() every component with part fields on an entity and everything below
 * it.
 *
 * @param root the top entity (a prefab's root)
 * @param skip when given, an entity it returns true for is passed over, with
 *             everything below it (e.g. a nested prefab's instance, whose own
 *             file holds its parts)
 * @return how many fields changed
 */
std::size_t fillAll(const sfs::Entity& root,
                    const std::function<bool(const sfs::Entity&)>& skip = {});

/**
 * Check the part fields on an entity and everything below it, as a prefab is
 * opened or applied: one whose target is no longer named as its part, or
 * whose saved target didn't resolve (its node renamed in the source model),
 * is filled again by name, and logged. One whose part is no longer there
 * keeps its value (logged). Empty fields are left empty.
 *
 * @param root   the top entity (a prefab's root)
 * @param source what is checked, for messages (e.g. the prefab's path)
 * @return how many fields changed
 */
std::size_t heal(const sfs::Entity& root, const std::string& source);

} // namespace parts
