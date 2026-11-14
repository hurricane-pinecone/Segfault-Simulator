#include "scene/references.h"

#include "scene/entityLabel.h"

#include "engine/core/components/sceneIdComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/ecs/registry.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/serialization/sceneSerializer.h"
#include "engine/runtime/serialization/unresolvedComponents.h"

#include <unordered_map>

namespace references
{

using sfs::SceneSerializer;
using sfs::reflect::FieldInfo;
using sfs::reflect::FieldKind;
using sfs::reflect::TypeInfo;

namespace
{
// The entity a reference names: the field's value, or a list's item.
sfs::Entity& slotOf(const FieldInfo& field, void* object, uint32_t item)
{
  if (field.kind == FieldKind::Entity)
    return field.in<sfs::Entity>(object);
  return field.in<std::vector<sfs::Entity>>(object)[item];
}
} // namespace

sfs::Entity contextRoot(const sfs::Entity& entity, bool prefabContent)
{
  if (!entity.isValid())
    return {};
  const sfs::Entity root = SceneSerializer::entryRootOf(entity);
  if (root.isValid() || !prefabContent)
    return root;
  return sfs::hierarchy::rootOf(entity);
}

const std::string* keptValue(const sfs::Entity& entity,
                             const TypeInfo& type,
                             const FieldInfo& field,
                             uint32_t item)
{
  const nlohmann::json* kept =
      SceneSerializer::keptReference(entity, type, field, item);
  return kept ? &kept->get_ref<const std::string&>() : nullptr;
}

void forgetKept(const sfs::Entity& entity,
                const TypeInfo& type,
                const FieldInfo& field,
                uint32_t item)
{
  nlohmann::json* kept = SceneSerializer::keptReferences(entity, type, field);
  if (!kept)
    return;
  if (item == kNoItem)
    *kept = nullptr;
  else if (kept->is_array() && item < kept->size())
    (*kept)[item] = nullptr;
}

void eraseKeptItem(const sfs::Entity& entity,
                   const TypeInfo& type,
                   const FieldInfo& field,
                   std::size_t item)
{
  nlohmann::json* kept = SceneSerializer::keptReferences(entity, type, field);
  if (kept && kept->is_array() && item < kept->size())
    kept->erase(kept->begin() + static_cast<std::ptrdiff_t>(item));
}

nlohmann::json comparable(const sfs::Entity& holder,
                          const TypeInfo& type,
                          const FieldInfo& field,
                          void* object,
                          bool prefabContent)
{
  const sfs::Entity root = contextRoot(holder, prefabContent);
  const auto one = [&](const sfs::Entity& target,
                       uint32_t item) -> nlohmann::json
  {
    if (target.isValid())
    {
      if (const std::optional<std::string> path =
              SceneSerializer::pathInInstance(target, root))
        return *path;
      return target.getId();
    }
    if (const std::string* kept = keptValue(holder, type, field, item))
      return {{"missing", *kept}};
    return nullptr;
  };
  if (field.kind == FieldKind::Entity)
    return one(field.in<sfs::Entity>(object), kNoItem);
  nlohmann::json list = nlohmann::json::array();
  const auto& targets = field.in<std::vector<sfs::Entity>>(object);
  for (std::size_t i = 0; i < targets.size(); ++i)
    list.push_back(one(targets[i], static_cast<uint32_t>(i)));
  return list;
}

std::optional<sfs::Entity> copied(const sfs::Entity& target,
                                  const sfs::Entity& from,
                                  const sfs::Entity& to,
                                  bool prefabContent)
{
  if (!target.isValid())
    return target;
  const sfs::Entity fromRoot = contextRoot(from, prefabContent);
  const std::optional<std::string> path =
      SceneSerializer::pathInInstance(target, fromRoot);
  if (!path)
    return target; // outside its root: the same entity for every object
  const sfs::Entity root = contextRoot(to, prefabContent);
  const sfs::Entity found = SceneSerializer::entityAtPath(root, *path);
  if (!found.isValid())
  {
    LOG_WARN("Copying a reference to " + entityLabel(target) + ": " +
             entityLabel(to) + "'s " + entityLabel(root) +
             " has no entity at its place (" + namePath(target, fromRoot) +
             "); left as it was");
    return std::nullopt;
  }
  return found;
}

bool copy(const TypeInfo& type,
          const FieldInfo& field,
          const sfs::Entity& from,
          void* fromObject,
          const sfs::Entity& to,
          void* toObject,
          bool prefabContent)
{
  if (field.kind == FieldKind::Entity)
  {
    const std::optional<sfs::Entity> target =
        copied(field.in<sfs::Entity>(fromObject), from, to, prefabContent);
    if (!target)
      return false;
    field.in<sfs::Entity>(toObject) = *target;
  }
  else
  {
    const auto& source = field.in<std::vector<sfs::Entity>>(fromObject);
    std::vector<sfs::Entity> targets;
    targets.reserve(source.size());
    for (const sfs::Entity& each : source)
    {
      const std::optional<sfs::Entity> target =
          copied(each, from, to, prefabContent);
      if (!target)
        return false;
      targets.push_back(*target);
    }
    field.in<std::vector<sfs::Entity>>(toObject) = std::move(targets);
  }

  // What's kept for empty items goes with them: a cleared one forgets it, and
  // a list's stays with its items.
  const nlohmann::json* kept =
      SceneSerializer::keptReferences(from, type, field);
  if (nlohmann::json* own = SceneSerializer::keptReferences(to, type, field))
    *own = kept ? *kept : nlohmann::json();
  else if (kept && !kept->is_null() && to.isValid())
  {
    sfs::Entity holder = to;
    if (!holder.hasComponent<sfs::UnresolvedComponents>())
      holder.addComponent<sfs::UnresolvedComponents>();
    holder.getComponent<sfs::UnresolvedComponents>()
        ->refs[type.name][field.name] = *kept;
  }
  return true;
}

std::vector<Inbound> inboundTo(const sfs::Entity& root)
{
  std::vector<Inbound> out;
  sfs::Registry* registry = root.isValid() ? root.registryOf() : nullptr;
  if (!registry)
    return out;
  const auto inside = [&](const sfs::Entity& entity)
  { return entity == root || sfs::hierarchy::isAncestor(root, entity); };
  for (const TypeInfo* type : sfs::reflect::types().referencingTypes())
  {
    if (!type->component.holders)
      continue;
    for (const sfs::Entity& holder : type->component.holders(*registry))
    {
      if (inside(holder))
        continue; // the instance's own: its entry names them
      void* object = type->component.get(holder);
      if (!object)
        continue;
      const auto note =
          [&](const FieldInfo& field, uint32_t item, const sfs::Entity& target)
      {
        if (!target.isValid() || !inside(target))
          return;
        Inbound ref{holder, type, &field, item};
        const auto& [through, path] = SceneSerializer::sceneHolderOf(target);
        if (sfs::SceneIdComponent* sceneId =
                through.getComponent<sfs::SceneIdComponent>())
        {
          ref.sceneId = sceneId->id;
          ref.path = path;
        }
        else if (const std::optional<std::string> fromRoot =
                     SceneSerializer::pathInInstance(target, root))
          ref.path = *fromRoot;
        else
        {
          LOG_WARN(entityLabel(holder) + " " + type->name + "." + field.name +
                   " names " + entityLabel(target) +
                   ", which its instance can't name again once rebuilt");
          return;
        }
        out.push_back(std::move(ref));
      };
      for (const FieldInfo& field : type->fields)
        if (field.kind == FieldKind::Entity)
          note(field, kNoItem, field.in<sfs::Entity>(object));
        else if (field.kind == FieldKind::EntityList)
        {
          const auto& targets = field.in<std::vector<sfs::Entity>>(object);
          for (std::size_t i = 0; i < targets.size(); ++i)
            note(field, static_cast<uint32_t>(i), targets[i]);
        }
    }
  }
  return out;
}

void restore(const std::vector<Inbound>& refs,
             const sfs::Entity& root,
             sfs::Scene& scene)
{
  std::optional<std::unordered_map<uint32_t, sfs::Entity>> byId;
  for (const Inbound& ref : refs)
  {
    void* object =
        ref.holder.isValid() ? ref.type->component.get(ref.holder) : nullptr;
    if (!object)
      continue;
    if (ref.item != kNoItem &&
        ref.item >= ref.field->in<std::vector<sfs::Entity>>(object).size())
      continue;
    sfs::Entity through = root;
    if (ref.sceneId)
    {
      if (!byId)
        byId = SceneSerializer::sceneIdHolders(scene);
      const auto it = byId->find(*ref.sceneId);
      through = it != byId->end() ? it->second : sfs::Entity();
    }
    const sfs::Entity target =
        through.isValid() ? SceneSerializer::entityAtPath(through, ref.path)
                          : sfs::Entity();
    if (!target.isValid())
      LOG_WARN(entityLabel(ref.holder) + " " + ref.type->name + "." +
               ref.field->name + ": its target is gone from " +
               entityLabel(root) + "; left empty");
    slotOf(*ref.field, object, ref.item) = target;
  }
}

std::string namePath(const sfs::Entity& entity, const sfs::Entity& root)
{
  std::vector<std::string> names;
  for (sfs::Entity at = entity; at.isValid() && at != root;
       at = sfs::hierarchy::parentOf(at))
    names.push_back(entityLabel(at));
  std::string path;
  for (auto it = names.rbegin(); it != names.rend(); ++it)
    path += (path.empty() ? "" : "/") + *it;
  return path;
}

} // namespace references
