// SceneSerializer's entity references: saving an Entity field as a path in
// the context it's written in, and resolving saved paths once a load has made
// every entity. See sceneSerializer.h for the forms.

#include "engine/runtime/serialization/sceneSerializer.h"

#include "engine/core/components/nameComponent.h"
#include "engine/core/components/sceneIdComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/core/util/debugFeatures.h"
#include "engine/core/util/random.h"
#include "engine/core/util/string.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/serialization/prefabComponents.h"
#include "engine/runtime/serialization/unresolvedComponents.h"
#include "entityRefPaths.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <vector>

namespace sfs
{

using json = nlohmann::json;

namespace entityRefs
{

std::string followPath(std::string_view base, std::string_view path)
{
  std::string key(base);
  const bool inside =
      forEachSegment(path,
                     [&](std::string_view segment)
                     {
                       if (segment != "..")
                       {
                         if (!key.empty())
                           key += '/';
                         key += segment;
                         return true;
                       }
                       if (key.empty())
                         return false;
                       const std::size_t last = key.rfind('/');
                       key.resize(last == std::string::npos ? 0 : last);
                       return true;
                     });
  if (inside)
    return key;
  if (base.empty() || path.empty())
    return std::string(base.empty() ? path : base);
  std::string joined(base);
  joined += '/';
  joined += path;
  return joined;
}

std::string relativePath(std::string_view to, std::string_view from)
{
  // The segments both keys start with.
  std::size_t shared = 0; // characters of `to` (and `from`) they cover
  for (std::size_t i = 0;; ++i)
  {
    const bool toEnds = i == to.size() || to[i] == '/';
    const bool fromEnds = i == from.size() || from[i] == '/';
    if (toEnds && fromEnds)
    {
      if (i > 0)
        shared = i;
      if (i == to.size() || i == from.size())
        break;
      continue;
    }
    if (toEnds || fromEnds || to[i] != from[i])
      break;
  }

  std::string path;
  for (std::size_t i = shared; i < from.size(); ++i)
    if (from[i] != '/')
    {
      path += path.empty() ? ".." : "/..";
      i = std::min(from.find('/', i), from.size());
    }
  const std::string_view rest =
      to.substr(std::min(shared + (shared > 0 ? 1 : 0), to.size()));
  if (!rest.empty())
  {
    if (!path.empty())
      path += '/';
    path += rest;
  }
  return path;
}

} // namespace entityRefs

namespace
{

// An entity as messages name it.
std::string describe(const Entity& entity)
{
  if (NameComponent* name = entity.getComponent<NameComponent>())
    return "'" + name->name + "'";
  return "entity " + std::to_string(entity.getId());
}

// Whether a message keyed `key` hasn't been logged yet in this process (and
// now has): what every spawn of a prefab would repeat is logged once.
bool firstTime(std::string key)
{
  static std::mutex mutex;
  static std::set<std::string> logged;
  const std::lock_guard lock(mutex);
  return logged.insert(std::move(key)).second;
}

// The number a "/7/..." or "#3/..." value (without its first character)
// starts with, and the path after it.
std::optional<uint32_t> leadingNumber(std::string_view value,
                                      std::string_view& rest)
{
  const std::size_t slash = std::min(value.find('/'), value.size());
  rest = value.substr(std::min(slash + 1, value.size()));
  return entityRefs::segmentId(value.substr(0, slash));
}

// The scene ids kept data names, so a new one never takes them: "/7..."
// values kept unresolved, and the "$sceneId"s of kept entries (an instance
// whose prefab didn't load, added entities whose place is gone).
void keptSceneIds(const json& kept, std::unordered_set<uint32_t>& used)
{
  if (kept.is_object() || kept.is_array())
  {
    if (const std::optional<uint32_t> id =
            kept.is_object() ? entityRefs::sceneIdOf(kept) : std::nullopt)
      used.insert(*id);
    for (const json& item : kept)
      keptSceneIds(item, used);
    return;
  }
  if (!kept.is_string())
    return;
  const std::string& value = kept.get_ref<const std::string&>();
  std::string_view rest;
  if (value.starts_with('/'))
    if (const auto id = leadingNumber(std::string_view(value).substr(1), rest))
      used.insert(*id);
}

} // namespace

json* SceneSerializer::keptReferences(const Entity& entity,
                                      const reflect::TypeInfo& type,
                                      const reflect::FieldInfo& field)
{
  UnresolvedComponents* unresolvedComponents =
      entity.getComponent<UnresolvedComponents>();
  if (!unresolvedComponents)
    return nullptr;
  json& refs = unresolvedComponents->refs;
  if (!refs.is_object())
    return nullptr;
  const auto component = refs.find(type.name);
  if (component == refs.end() || !component->is_object())
    return nullptr;
  const auto value = component->find(field.name);
  return value == component->end() ? nullptr : &*value;
}

const json* SceneSerializer::keptReference(const Entity& entity,
                                           const reflect::TypeInfo& type,
                                           const reflect::FieldInfo& field,
                                           uint32_t item)
{
  const json* kept = keptReferences(entity, type, field);
  if (kept && item != kNoItem)
    kept = kept->is_array() && item < kept->size() ? &(*kept)[item] : nullptr;
  return kept && kept->is_string() ? kept : nullptr;
}

namespace
{
// The entity a relative value kept for `entity` was made relative to: its
// nearest instance root, or the top of its branch (a prefab's root while it's
// edited on its own is no longer an instance).
Entity keptBase(const Entity& entity)
{
  if (const Entity root = SceneSerializer::instanceRootOf(entity);
      root.isValid())
    return root;
  return hierarchy::rootOf(entity);
}

// A [[sfs::part]] field whose target isn't named as the part says (the model
// re-imported, its nodes renamed): logged once per source and field.
void checkPart(const SceneSerializer::PendingRef& ref, const Entity& target)
{
  const std::string& pattern = ref.field->attributes.part;
  if (pattern.empty() || !debugFeatures::enabled())
    return;
  const NameComponent* named = target.getComponent<NameComponent>();
  const std::string name = named ? named->name : std::string();
  if (matchesPart(name, pattern))
    return;
  if (!firstTime("part " + std::string(ref.source) + " " + ref.type->name +
                 "." + ref.field->name))
    return;
  LOG_WARN(std::string(ref.source) + ": " + ref.type->name + "." +
           ref.field->name + " names '" + name + "', which isn't its part \"" +
           pattern + "\" (stale part reference: model re-imported?)");
}

void setRef(const SceneSerializer::PendingRef& ref, const Entity& target)
{
  void* object = ref.type->component.get(ref.entity);
  if (!object)
    return;
  if (ref.item == SceneSerializer::kNoItem)
    ref.field->in<Entity>(object) = target;
  else if (auto& list = ref.field->in<std::vector<Entity>>(object);
           ref.item < list.size())
    list[ref.item] = target;
  checkPart(ref, target);
}

// Log a reference whose target doesn't exist (once per source, field and
// value: each spawn of a prefab would repeat it), and keep `kept` (when
// given) to be saved back while the field stays empty.
void failRef(const SceneSerializer::PendingRef& ref, const std::string* kept)
{
  if (firstTime("missing " + std::string(ref.source) + " " + ref.type->name +
                "." + ref.field->name + " " + ref.value))
    LOG_WARN(std::string(ref.source) + ": " + describe(ref.entity) + " " +
             ref.type->name + "." + ref.field->name + " -> no entity at '" +
             ref.value + "'; left empty");
  if (!kept || !ref.entity.isValid())
    return;
  Entity entity = ref.entity;
  if (!entity.hasComponent<UnresolvedComponents>())
    entity.addComponent<UnresolvedComponents>();
  json& slot = entity.getComponent<UnresolvedComponents>()
                   ->refs[ref.type->name][ref.field->name];
  if (ref.item == SceneSerializer::kNoItem)
  {
    slot = *kept;
    return;
  }
  if (!slot.is_array())
    slot = json::array();
  while (slot.size() <= ref.item)
    slot.push_back(nullptr);
  slot[ref.item] = *kept;
}
} // namespace

std::unordered_map<uint32_t, Entity>
SceneSerializer::sceneIdHolders(Scene& scene)
{
  std::unordered_map<uint32_t, Entity> holders;
  // References resolve while a scene loads, before its new entities are
  // flushed into queries.
  // sfs-ignore-next-line(filter-in-loop)
  for (const Entity& entity : scene.entities())
    if (SceneIdComponent* sceneId = entity.getComponent<SceneIdComponent>();
        sceneId && !holders.emplace(sceneId->id, entity).second)
      LOG_WARN("Two entities have the scene id " + std::to_string(sceneId->id) +
               "; references to it name the first");
  return holders;
}

std::pair<Entity, std::string>
SceneSerializer::sceneHolderOf(const Entity& target)
{
  std::pair<Entity, std::string> out{target, std::string()};
  for (Entity root = instanceRootOf(target); root.isValid();
       root = instanceRootOf(hierarchy::parentOf(root)))
  {
    std::optional<std::string> path = pathInInstance(target, root);
    if (!path)
      break; // an outer instance can't reach past what this one can't
    out = {root, std::move(*path)};
  }
  return out;
}

std::optional<uint32_t> SceneSerializer::sceneIdFor(const Entity& holder,
                                                    SaveOp* op)
{
  if (SceneIdComponent* sceneId = holder.getComponent<SceneIdComponent>())
    return sceneId->id;
  if (!op)
    return std::nullopt;
  if (!op->scanned)
  {
    // Once a save: every id the scene has or keeps.
    op->scanned = true;
    const auto gather = [&](const Entity& entity)
    {
      if (SceneIdComponent* sceneId = entity.getComponent<SceneIdComponent>())
        op->usedSceneIds.insert(sceneId->id);
      if (const UnresolvedComponents* kept =
              entity.getComponent<UnresolvedComponents>())
      {
        keptSceneIds(kept->refs, op->usedSceneIds);
        keptSceneIds(kept->overrides, op->usedSceneIds);
      }
      if (MissingPrefabComponent* missingPrefab =
              entity.getComponent<MissingPrefabComponent>())
        keptSceneIds(missingPrefab->entry, op->usedSceneIds);
    };
    for (const Entity& root : hierarchy::roots(*holder.registryOf()))
    {
      gather(root);
      for (const Entity& entity : hierarchy::descendants(root))
        gather(entity);
    }
  }
  uint32_t id = 0;
  while (id == 0 || !op->usedSceneIds.insert(id).second)
    id = static_cast<uint32_t>(random::next() & 0x7FFFFFFFu);
  Entity(holder).addComponent<SceneIdComponent>(SceneIdComponent{id});
  ++op->given;
  return id;
}

Entity SceneSerializer::entityAtPath(const Entity& root, std::string_view path)
{
  if (!root.isValid())
    return {};
  Entity at = root;
  const bool found = entityRefs::forEachSegment(
      path,
      [&](std::string_view segment)
      {
        const std::optional<uint32_t> id = entityRefs::segmentId(segment);
        if (!id)
          return false;
        // The entity with that id whose path goes through `at`: below it,
        // and not below an instance nested in it (those are the nested
        // one's).
        const hierarchy::DescendantRange below = hierarchy::descendants(at);
        for (auto it = below.begin(); it != below.end(); ++it)
        {
          const Entity entity = *it;
          if (PrefabLinkComponent* prefabLink =
                  entity.getComponent<PrefabLinkComponent>();
              prefabLink && prefabLink->id == *id)
          {
            at = entity;
            return true;
          }
          if (entity.hasComponent<PrefabInstanceComponent>())
            it.skipChildren();
        }
        return false;
      });
  return found ? at : Entity();
}

SceneSerializer::RefContext
SceneSerializer::instanceContext(const Entity& root, const RefContext& outer)
{
  RefContext refs = outer;
  // A prefab file names everything in it from its own root.
  if (!outer.sceneAllowed && outer.fileRoot.isValid())
    if (std::optional<std::string> path = pathInInstance(root, outer.fileRoot))
    {
      refs.fromKey = std::move(*path);
      return refs;
    }
  refs.fileRoot = root;
  refs.fromKey.clear();
  return refs;
}

Entity SceneSerializer::entryRootOf(const Entity& entity)
{
  // Its nearest instance root, or the one that root belongs to in turn (one
  // an instance merely holds as an added entity has its own entry).
  Entity root = instanceRootOf(entity);
  while (root.isValid())
  {
    const Entity up = instanceRootOf(hierarchy::parentOf(root));
    if (!up.isValid() || !pathInInstance(root, up))
      break;
    root = up;
  }
  return root;
}

SceneSerializer::RefContext SceneSerializer::sceneContext(const Entity& entity,
                                                          SaveOp& op)
{
  RefContext refs;
  refs.fileRoot = entryRootOf(entity);
  refs.sceneIds = true;
  refs.op = &op;
  return refs;
}

void SceneSerializer::forgetSetRefs(const Entity& entity,
                                    const reflect::TypeInfo& type,
                                    const reflect::FieldInfo& field,
                                    void* object) const
{
  json* kept = keptReferences(entity, type, field);
  if (!kept)
    return;
  if (field.kind == reflect::FieldKind::Entity)
  {
    if (field.in<Entity>(object) != Entity{})
      *kept = nullptr;
  }
  else if (kept->is_array())
  {
    // Kept items go with their places: a set or a gone one is dropped.
    const auto& targets = field.in<std::vector<Entity>>(object);
    if (kept->size() > targets.size())
      kept->erase(kept->begin() + static_cast<std::ptrdiff_t>(targets.size()),
                  kept->end());
    for (std::size_t i = 0; i < kept->size(); ++i)
      if (targets[i] != Entity{})
        (*kept)[i] = nullptr;
    if (std::all_of(kept->begin(),
                    kept->end(),
                    [](const json& item) { return item.is_null(); }))
      *kept = nullptr;
  }
  if (!kept->is_null())
    return;

  // Tidy up what's left empty, down to the component itself.
  Entity holder = entity;
  auto* unresolved = holder.getComponent<UnresolvedComponents>();
  json& component = unresolved->refs[type.name];
  component.erase(field.name);
  if (component.empty())
    unresolved->refs.erase(type.name);
  if (unresolved->refs.empty())
    unresolved->refs = nullptr;
  if (unresolved->refs.is_null() && unresolved->overrides.is_null() &&
      unresolved->components.empty())
    holder.removeComponent<UnresolvedComponents>();
}

json SceneSerializer::saveRef(const Entity& target,
                              const RefContext& refs,
                              const Entity& entity,
                              const reflect::TypeInfo& type,
                              const reflect::FieldInfo& field,
                              uint32_t item) const
{
  if (target == Entity{})
  {
    // Empty: what didn't resolve when it loaded is saved back as it was.
    const json* kept = keptReference(entity, type, field, item);
    if (!kept)
      return nullptr;
    const std::string& value = kept->get_ref<const std::string&>();
    if (refs.fileRoot.isValid() && entityRefs::isRelative(value))
      if (const std::optional<std::string> base =
              pathInInstance(keptBase(entity), refs.fileRoot))
        return entityRefs::relativePath(
            entityRefs::followPath(*base, value), refs.fromKey);
    return value;
  }
  // Set to an entity that's gone: saved as none.
  if (!target.isValid())
    return nullptr;

  if (refs.fileRoot.isValid())
    if (const std::optional<std::string> path =
            pathInInstance(target, refs.fileRoot))
      return entityRefs::relativePath(*path, refs.fromKey);

  const auto where = [&]
  {
    return describe(entity) + " " + type.name + "." + field.name + " -> " +
           describe(target);
  };
  if (refs.branch)
  {
    // An entry of the branch, or an entity in the instance one of them is.
    for (Entity at = target; at.isValid(); at = hierarchy::parentOf(at))
    {
      const auto numbered = refs.branch->find(at.getId());
      if (numbered == refs.branch->end())
        continue;
      const std::string entry = "#" + std::to_string(numbered->second);
      if (at == target)
        return entry;
      if (const std::optional<std::string> path = pathInInstance(target, at))
        return entry + "/" + *path;
      LOG_WARN("Copying " + where() +
               ": the copy can't name the target; left empty");
      return nullptr;
    }
  }
  if (refs.sceneAllowed)
  {
    const auto [holder, path] = sceneHolderOf(target);
    const std::optional<uint32_t> id = sceneIdFor(holder, refs.op);
    if (!id)
      return nullptr; // a read: it has no scene id yet
    std::string value = "/" + std::to_string(*id);
    if (!path.empty())
      value += "/" + path;
    return value;
  }
  LOG_WARN("Saving a prefab: " + where() +
           " is outside the prefab, which can't name it; saved as none");
  return nullptr;
}

void SceneSerializer::giveSceneIds(Scene& scene, SaveOp& op) const
{
  // The targets that will be written scene-absolutely, found through each
  // referencing type's holders: the cost follows the references, not the
  // scene. Ids are handed out after, so no list changes while it's read.
  // The scene's registry, through any of its entities (none: nothing to do).
  Registry* registry = nullptr;
  for (const Entity& root : scene.roots())
  {
    registry = root.registryOf();
    break;
  }
  if (!registry)
    return;
  std::vector<Entity> holders;
  for (const reflect::TypeInfo* type : m_types.referencingTypes())
  {
    if (!type->component.holders)
      continue;
    for (const Entity& entity : type->component.holders(*registry))
    {
      void* object = type->component.get(entity);
      if (!object)
        continue;
      std::optional<RefContext> refs;
      const auto need = [&](const Entity& target)
      {
        if (!target.isValid())
          return;
        if (!refs)
          refs = sceneContext(entity, op);
        if (refs->fileRoot.isValid() && pathInInstance(target, refs->fileRoot))
          return;
        if (const Entity holder = sceneHolderOf(target).first;
            !holder.hasComponent<SceneIdComponent>())
          holders.push_back(holder);
      };
      for (const reflect::FieldInfo& field : type->fields)
        if (field.kind == reflect::FieldKind::Entity)
          need(field.in<Entity>(object));
        else if (field.kind == reflect::FieldKind::EntityList)
          for (const Entity& target : field.in<std::vector<Entity>>(object))
            need(target);
    }
  }
  for (const Entity& holder : holders)
    sceneIdFor(holder, &op);
}

void SceneSerializer::resolveRefs(
    std::vector<PendingRef>& refs,
    Scene& scene,
    const std::vector<Entity>* entries,
    const std::unordered_map<std::string, Entity>* made,
    std::vector<PendingRef>* defer) const
{
  std::optional<std::unordered_map<uint32_t, Entity>> holders;
  for (PendingRef& ref : refs)
  {
    const std::string& value = ref.value;
    if (entityRefs::isRelative(value))
    {
      if (!made)
      {
        failRef(ref, &value); // only an instance's entry has a context root
        continue;
      }
      if (const auto it = made->find(value); it != made->end())
      {
        setRef(ref, it->second);
        continue;
      }
      // Kept relative to the entity's instance root, which a save finds again.
      std::string kept = value;
      const Entity base = instanceRootOf(ref.entity);
      for (const auto& [key, entity] : *made)
        if (entity == base)
        {
          kept = entityRefs::relativePath(value, key);
          break;
        }
      failRef(ref, &kept);
      continue;
    }

    const bool branch = value.front() == '#';
    if (defer && (made || !branch))
    {
      // A session outlives the entries it was given: its messages name the
      // scene.
      if (!made)
        ref.source = entityRefs::kSceneSource;
      defer->push_back(std::move(ref));
      continue;
    }
    std::string_view path;
    const std::optional<uint32_t> number =
        leadingNumber(std::string_view(value).substr(1), path);
    Entity top;
    if (branch)
    {
      if (entries && number && *number < entries->size())
        top = (*entries)[*number];
    }
    else if (number)
    {
      if (!holders)
        holders = sceneIdHolders(scene);
      if (const auto it = holders->find(*number); it != holders->end())
        top = it->second;
    }
    const Entity target =
        top.isValid() && !path.empty() ? entityAtPath(top, path) : top;
    if (target.isValid())
      setRef(ref, target);
    else
      failRef(ref, branch ? nullptr : &value); // "#n" is never in a file
  }
  refs.clear();
}

Entity SceneSerializer::loadEntity(const json& entry,
                                   Scene& scene,
                                   const Entity& parent,
                                   LoadSession* session) const
{
  std::vector<Entity> entries;
  LoadOp op;
  op.entries = &entries;
  const Entity top = loadEntity(entry, scene, parent, op);
  resolveRefs(
      op.refs, scene, &entries, nullptr, session ? &session->m_refs : nullptr);
  return top;
}

void SceneSerializer::finish(LoadSession& session, Scene& scene) const
{
  resolveRefs(session.m_refs, scene, nullptr, nullptr, nullptr);
}

} // namespace sfs
