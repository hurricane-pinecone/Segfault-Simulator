#include "scene/parts.h"

#include "scene/entityLabel.h"
#include "scene/references.h"

#include "engine/core/components/nameComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/core/util/string.h"

#include <algorithm>
#include <functional>
#include <vector>

namespace parts
{

using sfs::reflect::FieldInfo;
using sfs::reflect::FieldKind;
using sfs::reflect::TypeInfo;

namespace
{
bool isPart(const FieldInfo& field)
{
  return sfs::reflect::isEntityField(field) && !field.attributes.part.empty();
}

bool named(const sfs::Entity& entity, const std::string& pattern)
{
  return entity.isValid() && sfs::matchesPart(entityLabel(entity), pattern);
}

// The entities below `entity` named as `pattern` says, in preorder.
std::vector<sfs::Entity> matches(const sfs::Entity& entity,
                                 const std::string& pattern)
{
  std::vector<sfs::Entity> found;
  for (const sfs::Entity& below : sfs::hierarchy::descendants(entity))
    if (named(below, pattern))
      found.push_back(below);
  return found;
}

std::string describe(const sfs::Entity& entity,
                     const TypeInfo& type,
                     const FieldInfo& field)
{
  return entityLabel(entity) + " " + type.name + "." + field.name;
}

// What a field is filled with: the matches (one for an Entity field). False
// (logged) if there are none.
bool lookUp(const sfs::Entity& entity,
            const TypeInfo& type,
            const FieldInfo& field,
            std::vector<sfs::Entity>& found)
{
  const std::string& pattern = field.attributes.part;
  found = matches(entity, pattern);
  if (found.empty())
  {
    LOG_WARN(describe(entity, type, field) + ": nothing below " +
             entityLabel(entity) + " is named as its part \"" + pattern +
             "\"; left as it was");
    return false;
  }
  if (field.kind == FieldKind::Entity && found.size() > 1)
    LOG_WARN(describe(entity, type, field) + ": " +
             std::to_string(found.size()) + " entities below " +
             entityLabel(entity) + " are named as its part \"" + pattern +
             "\"; it takes the first, " +
             references::namePath(found.front(), entity));
  if (field.kind == FieldKind::Entity)
    found.resize(1);
  return true;
}

// Set a field to what lookUp() found. True if that changed it.
bool set(const sfs::Entity& entity,
         const TypeInfo& type,
         const FieldInfo& field,
         void* object,
         const std::vector<sfs::Entity>& found)
{
  bool changed = false;
  if (field.kind == FieldKind::Entity)
  {
    sfs::Entity& value = field.in<sfs::Entity>(object);
    changed = value != found.front();
    value = found.front();
  }
  else
  {
    auto& list = field.in<std::vector<sfs::Entity>>(object);
    changed = list != found;
    list = found;
  }
  if (!changed)
    return false;
  std::string names;
  for (const sfs::Entity& target : found)
    names += (names.empty() ? "" : ", ") + references::namePath(target, entity);
  LOG_INFO(describe(entity, type, field) + " -> " + names);
  return true;
}

bool empty(const FieldInfo& field, void* object)
{
  if (field.kind == FieldKind::Entity)
    return field.in<sfs::Entity>(object) == sfs::Entity{};
  const auto& list = field.in<std::vector<sfs::Entity>>(object);
  return std::none_of(list.begin(),
                      list.end(),
                      [](const sfs::Entity& item)
                      { return item != sfs::Entity{}; });
}

// Whether a part field no longer names its part: a target named otherwise, or
// a saved target that didn't resolve.
bool stale(const sfs::Entity& entity,
           const TypeInfo& type,
           const FieldInfo& field,
           void* object)
{
  const std::string& pattern = field.attributes.part;
  const auto wrong = [&](const sfs::Entity& target, uint32_t item)
  {
    // A target that's set but no longer lives is wrong too: healed by name.
    return target != sfs::Entity{}
               ? !named(target, pattern)
               : references::keptValue(entity, type, field, item) != nullptr;
  };
  if (field.kind == FieldKind::Entity)
    return wrong(field.in<sfs::Entity>(object), references::kNoItem);
  const auto& list = field.in<std::vector<sfs::Entity>>(object);
  for (std::size_t i = 0; i < list.size(); ++i)
    if (wrong(list[i], static_cast<uint32_t>(i)))
      return true;
  return false;
}

// Every component with part fields on `root` and below it, but not at or
// below an entity `skip` returns true for.
template <typename Visit>
void eachWithParts(const sfs::Entity& root,
                   Visit&& visit,
                   const std::function<bool(const sfs::Entity&)>& skip = {})
{
  std::vector<const TypeInfo*> types;
  for (const TypeInfo* type : sfs::reflect::types().referencingTypes())
    if (hasParts(*type))
      types.push_back(type);
  if (types.empty() || !root.isValid())
    return;
  const auto at = [&](const sfs::Entity& entity)
  {
    for (const TypeInfo* type : types)
      if (type->component.has(entity))
        visit(entity, *type);
  };
  at(root);
  const sfs::hierarchy::DescendantRange below =
      sfs::hierarchy::descendants(root);
  for (auto it = below.begin(); it != below.end(); ++it)
    if (skip && skip(*it))
      it.skipChildren();
    else
      at(*it);
}
} // namespace

bool hasParts(const TypeInfo& type)
{
  return std::any_of(type.fields.begin(), type.fields.end(), isPart);
}

std::size_t
fill(const sfs::Entity& entity, const TypeInfo& type, bool onlyEmpty)
{
  void* object = type.component.get(entity);
  if (!object)
    return 0;
  std::size_t changed = 0;
  std::vector<sfs::Entity> found;
  for (const FieldInfo& field : type.fields)
    if (isPart(field) && (!onlyEmpty || empty(field, object)) &&
        lookUp(entity, type, field, found) &&
        set(entity, type, field, object, found))
      ++changed;
  return changed;
}

std::size_t fillAll(const sfs::Entity& root,
                    const std::function<bool(const sfs::Entity&)>& skip)
{
  std::size_t changed = 0;
  eachWithParts(
      root,
      [&](const sfs::Entity& entity, const TypeInfo& type)
      { changed += fill(entity, type); },
      skip);
  return changed;
}

std::size_t heal(const sfs::Entity& root, const std::string& source)
{
  std::size_t changed = 0;
  std::vector<sfs::Entity> found;
  eachWithParts(
      root,
      [&](const sfs::Entity& entity, const TypeInfo& type)
      {
        void* object = type.component.get(entity);
        for (const FieldInfo& field : type.fields)
          if (isPart(field) && stale(entity, type, field, object))
          {
            LOG_INFO(source + ": " + describe(entity, type, field) +
                     " no longer names its part \"" + field.attributes.part +
                     "\" (a node renamed in the model?); filling it again");
            if (lookUp(entity, type, field, found) &&
                set(entity, type, field, object, found))
              ++changed;
          }
      });
  return changed;
}

} // namespace parts
