// SceneSerializer's prefab side: expanding instances (with nesting and
// overrides) into entities, and saving an instance back as its prefab plus how
// it differs. See sceneSerializer.h for the format.

#include "engine/runtime/serialization/sceneSerializer.h"

#include "engine/core/ecs/registry.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/core/util/hash.h"
#include "engine/core/util/jsonTokens.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/serialization/prefabComponents.h"
#include "engine/runtime/serialization/unresolvedComponents.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <unordered_set>

namespace sfs
{

using json = nlohmann::json;

namespace
{
constexpr const char* kChildrenKey = "children";
constexpr int kMaxNesting =
    16; // deeper than this is a prefab containing itself
// The first prefab format whose model prefabs give their nodes stable ids.
constexpr int kStableNodeIdFormat = 2;
// What an old model node id that's past the model's nodes becomes ("old:9"),
// so it's kept as it was and can never name a node again.
constexpr std::string_view kOldNodeIdPrefix = "old:";

std::string join(const std::string& path, const std::string& part)
{
  if (part.empty())
    return path; // "": the instance's own root
  return path.empty() ? part : path + "/" + part;
}

std::optional<uint32_t> idOf(const json& entry)
{
  const auto it = entry.find("$id");
  if (it == entry.end() || !it->is_number_integer() || it->get<int64_t>() < 0)
    return std::nullopt;
  return static_cast<uint32_t>(it->get<int64_t>());
}

// A file's "formatVersion" (0 if it has none).
int formatOf(const json& file)
{
  const auto it = file.find("formatVersion");
  return it != file.end() && it->is_number_integer() ? it->get<int>() : 0;
}

// A path segment as an id; nothing if it isn't one.
std::optional<uint32_t> segmentId(std::string_view segment)
{
  uint32_t id = 0;
  const auto [end, error] =
      std::from_chars(segment.data(), segment.data() + segment.size(), id);
  if (segment.empty() || error != std::errc() ||
      end != segment.data() + segment.size())
    return std::nullopt;
  return id;
}

// The id of an entity a prefab adds inside one of its nested instances, when
// its file gives it none: made from the id of the instance (or entity) whose
// entry holds it and its place there, so it's the same at every load.
uint32_t addedNodeId(uint32_t holder, char separator, std::size_t index)
{
  char digits[24];
  uint64_t hash = fnv1a64(std::string_view(
      digits,
      static_cast<std::size_t>(
          std::to_chars(digits, digits + sizeof(digits), holder).ptr -
          digits)));
  hash = fnv1a64(std::string_view(&separator, 1), hash);
  hash = fnv1a64(
      std::string_view(
          digits,
          static_cast<std::size_t>(
              std::to_chars(digits, digits + sizeof(digits), index).ptr -
              digits)),
      hash);
  return kAddedNodeIdBit |
         static_cast<uint32_t>(mix64(hash) & (kAddedNodeIdBit - 1));
}

// An entry's components: everything but children and "$" keys.
json componentsOf(const json& entry)
{
  json out = json::object();
  for (const auto& [key, value] : entry.items())
    if (key != kChildrenKey && !key.starts_with('$'))
      out[key] = value;
  return out;
}

// RFC 7386: objects merge, null removes, anything else replaces.
void mergePatch(json& target, const json& patch)
{
  if (!patch.is_object())
  {
    target = patch;
    return;
  }
  if (!target.is_object())
    target = json::object();
  for (const auto& [key, value] : patch.items())
  {
    if (value.is_null())
      target.erase(key);
    else
      mergePatch(target[key], value);
  }
}

bool sameValue(const json& a, const json& b)
{
  if (a.is_number() && b.is_number())
  {
    const double x = a.get<double>(), y = b.get<double>();
    return std::abs(x - y) <= 1.0e-5 * std::max(1.0, std::abs(x));
  }
  if (a.is_array() && b.is_array())
  {
    if (a.size() != b.size())
      return false;
    for (std::size_t i = 0; i < a.size(); ++i)
      if (!sameValue(a[i], b[i]))
        return false;
    return true;
  }
  return a == b;
}

// The merge patch that turns `base` into `actual` (empty when they match).
json mergeDiff(const json& base, const json& actual)
{
  json patch = json::object();
  for (const auto& [key, value] : actual.items())
  {
    const auto it = base.find(key);
    if (it == base.end())
      patch[key] = value;
    else if (it->is_object() && value.is_object())
    {
      json inner = mergeDiff(*it, value);
      if (!inner.empty())
        patch[key] = std::move(inner);
    }
    else if (!sameValue(*it, value))
      patch[key] = value;
  }
  for (const auto& [key, value] : base.items())
    if (!actual.contains(key))
      patch[key] = nullptr;
  return patch;
}
} // namespace

// One entity of an expanded instance.
struct SceneSerializer::PrefabNode
{
  std::string
      key; // its path in the instance ("+n" segments: not from the prefab)
  std::string parent; // its parent's key; the top node's is ignored
  json components = json::object();
  std::optional<uint32_t> link; // its id in the file that made it
  std::string prefab;           // set: the root of an instance of this
  json missing; // set: an instance whose prefab didn't load (its entry)
  bool removed = false;
  bool model = false; // made from a model's prefab: its rest pose is recorded
  // Set on the root of an instance of a model prefab with stable ids: how
  // many nodes the model gave it. They follow it in `nodes`, in the model
  // file's node order, which is what an older file's ids into it count.
  std::optional<uint32_t> modelNodes;
  // An instance root: the parts of its entry naming entities its prefab
  // doesn't have (see UnresolvedComponents::overrides); null if none.
  json unused;
};

namespace
{
// Expands instance entries into PrefabNodes, parents first.
class Expander
{
public:
  using Node = SceneSerializer::PrefabNode;
  using NodeIdEdit = SceneSerializer::NodeIdEdit;

  // `warn`: log each instance's unused overrides (when loading). `edits`:
  // record the old model node ids translated in entries that have a pointer
  // (those of the file being upgraded). `unreadable`: gets the name of each
  // prefab an instance names that can't be expanded.
  explicit Expander(const SceneSerializer::PrefabSource& source,
                    bool warn = false,
                    std::vector<NodeIdEdit>* edits = nullptr,
                    std::vector<std::string>* unreadable = nullptr)
      : m_source(source), m_warn(warn), m_edits(edits), m_unreadable(unreadable)
  {
  }

  std::vector<Node> nodes;

  // `link`: the instance's id in the prefab file holding its entry (none for a
  // scene's own instance). What a held entry adds then belongs to that prefab,
  // and gets ids. `pointer`: the entry's JSON pointer in the file being
  // upgraded (null for entries of other files, e.g. the prefabs it expands).
  void instance(const json& entry,
                const std::string& key,
                const std::string& parent,
                std::optional<uint32_t> link,
                const std::string* pointer = nullptr)
  {
    const auto prefab = entry.find("$prefab");
    const std::string name = prefab != entry.end() && prefab->is_string()
                                 ? prefab->get<std::string>()
                                 : std::string();
    std::optional<json> data;
    const bool cycle =
        std::find(m_stack.begin(), m_stack.end(), name) != m_stack.end() ||
        m_stack.size() >= kMaxNesting;
    if (cycle)
      LOG_ERROR("Prefab " + name + " contains itself; that copy is left out");
    else if (m_source && !name.empty())
      data = m_source(name);
    if (!data || !data->is_object() || !data->contains("root") ||
        !(*data)["root"].is_object())
    {
      const std::string label = !name.empty()           ? name
                                : prefab != entry.end() ? prefab->dump()
                                                        : std::string("(none)");
      if (!cycle)
        LOG_ERROR("Can't read the prefab " + label +
                  "; its instance is kept as saved");
      if (m_unreadable)
        m_unreadable->push_back(label);
      add({key, parent, json::object(), link, name, entry});
      return;
    }

    m_stack.push_back(name);
    const std::size_t first = nodes.size();
    content((*data)["root"], key, parent, link, key);
    const std::size_t root = m_index[key];
    nodes[root].prefab = name;
    if (const auto model = data->find("model");
        model != data->end() && model->is_boolean() && model->get<bool>())
    {
      for (std::size_t i = first; i < nodes.size(); ++i)
        nodes[i].model = true;
      if (root == first && formatOf(*data) >= kStableNodeIdFormat)
        nodes[root].modelNodes =
            static_cast<uint32_t>(nodes.size() - first - 1);
    }

    // How this instance differs, on top of the prefab (and its own nested
    // instances' differences, already applied). What names nothing here is
    // kept unused, with its place in its list so it's saved back there.
    json unused;
    if (const auto overrides = entry.find("$overrides");
        overrides != entry.end() && overrides->is_object())
      for (const auto& [path, patch] : overrides->items())
      {
        const std::string at = pathKey(
            key,
            path,
            pointer,
            [&] { return "/$overrides/" + jsonPointerSegment(path); },
            true);
        if (const auto it = m_index.find(at); it == m_index.end())
          unused["$overrides"][unusedPath(key, at)] = patch;
        else if (!nodes[it->second].removed)
          mergePatch(nodes[it->second].components, patch);
      }
    if (const auto removed = entry.find("$removed");
        removed != entry.end() && removed->is_array())
      for (std::size_t i = 0; i < removed->size(); ++i)
      {
        const json& path = (*removed)[i];
        if (!path.is_string())
          continue;
        const std::string at = pathKey(
            key,
            path.get_ref<const std::string&>(),
            pointer,
            [&] { return "/$removed/" + std::to_string(i); },
            false);
        if (m_index.contains(at))
          remove(at);
        else
        {
          unused["$removed"].push_back(unusedPath(key, at));
          unused["$removedAt"].push_back(i);
        }
      }
    if (const auto added = entry.find("$added");
        added != entry.end() && added->is_array())
      for (std::size_t i = 0; i < added->size(); ++i)
      {
        const json& extra = (*added)[i];
        if (!extra.is_object())
          continue;
        const auto parentPath = extra.find("$parent");
        static const std::string kNone;
        const std::string parentKey = pathKey(
            key,
            parentPath != extra.end() && parentPath->is_string()
                ? parentPath->get_ref<const std::string&>()
                : kNone,
            pointer,
            [&] { return "/$added/" + std::to_string(i) + "/$parent"; },
            false);
        const auto above = m_index.find(parentKey);
        if (above == m_index.end())
        {
          json kept = extra;
          if (parentPath != extra.end())
            kept["$parent"] = unusedPath(key, parentKey);
          unused["$added"].push_back(std::move(kept));
          unused["$addedAt"].push_back(i);
          continue;
        }
        if (nodes[above->second].removed)
          continue;
        const std::string extraPointer =
            pointer ? *pointer + "/$added/" + std::to_string(i) : std::string();
        addEntity(extra,
                  key,
                  parentKey,
                  link ? std::optional<uint32_t>(addedId(extra, *link, '+', i))
                       : std::nullopt,
                  pointer ? &extraPointer : nullptr);
      }
    if (!unused.is_null())
    {
      if (m_warn)
        warnUnused(name, unused);
      nodes[root].unused = std::move(unused);
    }
    m_stack.pop_back();
  }

  // A prefab's (or scene's) entry: entities with "$id"s are keyed by them under
  // `owner` (the instance they belong to); others get "+n" keys, or with
  // `madeIds` (inside what a prefab adds to a nested instance) ids made from
  // their parent's. `pointer`: as instance()'s.
  void content(const json& entry,
               const std::string& key,
               const std::string& parent,
               std::optional<uint32_t> link,
               const std::string& owner,
               const std::string* pointer = nullptr,
               bool madeIds = false)
  {
    if (entry.contains("$prefab"))
    {
      instance(entry, key, parent, link, pointer);
      return;
    }
    add({key, parent, componentsOf(entry), link});
    if (const auto children = entry.find(kChildrenKey);
        children != entry.end() && children->is_array())
      for (std::size_t i = 0; i < children->size(); ++i)
      {
        const json& child = (*children)[i];
        if (!child.is_object())
          continue;
        const std::string childPointer =
            pointer ? *pointer + "/" + kChildrenKey + "/" + std::to_string(i)
                    : std::string();
        if (madeIds && link)
        {
          addEntity(child,
                    key,
                    key,
                    addedId(child, *link, '/', i),
                    pointer ? &childPointer : nullptr);
          continue;
        }
        const std::optional<uint32_t> childId = idOf(child);
        const std::string childKey =
            childId ? join(owner, std::to_string(*childId))
                    : join(key, "+" + std::to_string(m_added++));
        content(child,
                childKey,
                key,
                childId,
                childId ? owner : childKey,
                pointer ? &childPointer : nullptr);
      }
  }

private:
  // The id an entity a prefab adds inside a nested instance gets: its own
  // "$id" if its file gives one, else one made from its place.
  static uint32_t
  addedId(const json& entry, uint32_t holder, char separator, std::size_t index)
  {
    const std::optional<uint32_t> id = idOf(entry);
    return id && (*id & kAddedNodeIdBit) != 0 && (*id & kModelNodeIdBit) == 0
               ? *id
               : addedNodeId(holder, separator, index);
  }

  // An entity added under `parentKey` by the entry of the instance at `key`.
  // With an `id` (the entry is a prefab's), it belongs to that prefab: keyed
  // by the id under the instance its parent is in, as pathInInstance() finds
  // it. Without one (a scene's own), it gets a "+n" key.
  void addEntity(const json& entry,
                 const std::string& key,
                 const std::string& parentKey,
                 std::optional<uint32_t> id,
                 const std::string* pointer)
  {
    std::string owner;
    std::string entityKey;
    if (id)
    {
      owner = ownerOf(parentKey);
      entityKey = join(owner, std::to_string(*id));
      if (m_index.contains(entityKey))
      {
        LOG_WARN("Prefab entity " + entityKey +
                 " is added twice; the copy is kept as the instance's own");
        id.reset();
      }
    }
    if (!id)
      owner = entityKey = join(key, "+" + std::to_string(m_added++));
    if (entry.contains("$prefab"))
      instance(entry, entityKey, parentKey, id, pointer);
    else
      content(entry, entityKey, parentKey, id, owner, pointer, id.has_value());
  }

  // The key of the instance an entity's children belong to: the entity's own
  // if it's an instance root, else the one it belongs to.
  std::string ownerOf(const std::string& key) const
  {
    if (const auto it = m_index.find(key); it == m_index.end() ||
                                           !nodes[it->second].prefab.empty() ||
                                           !nodes[it->second].missing.is_null())
      return key;
    const std::size_t slash = key.rfind('/');
    return slash == std::string::npos ? std::string() : key.substr(0, slash);
  }

  // An unused path as it's kept and saved: relative to the instance at `key`.
  static std::string unusedPath(const std::string& key, const std::string& at)
  {
    return key.empty() ? at : at.substr(std::min(at.size(), key.size() + 1));
  }

  // The key of the entity a path names, the path written in the entry of the
  // instance at `key`. A segment naming a model's node by its old id (its
  // place in the model file's order, below kAddedNodeIdBit) becomes the
  // node's stable id, or "old:<id>" if the model has no node there. When that
  // changes the path and `pointer` is set, the edit is recorded, at `pointer`
  // + `where()`.
  template <typename Where>
  std::string pathKey(const std::string& key,
                      std::string_view path,
                      const std::string* pointer,
                      const Where& where,
                      bool isKey)
  {
    std::string at;
    at.reserve(key.size() + path.size() + 1);
    at = key;
    std::optional<uint32_t> fromId;
    uint32_t toId = 0;
    std::string model;
    for (std::size_t start = 0; start < path.size();)
    {
      const std::size_t slash = std::min(path.find('/', start), path.size());
      std::string_view segment = path.substr(start, slash - start);
      start = slash + 1;
      if (segment.empty())
        continue;
      bool past = false;
      if (const std::optional<uint32_t> old = segmentId(segment);
          old && *old < kAddedNodeIdBit)
        if (const auto owner = m_index.find(at);
            owner != m_index.end() && nodes[owner->second].modelNodes)
        {
          fromId = *old;
          model = nodes[owner->second].prefab;
          past = *old < 1 || *old > *nodes[owner->second].modelNodes;
          if (!past)
          {
            const std::string& node = nodes[owner->second + *old].key;
            segment = std::string_view(node).substr(node.rfind('/') + 1);
            toId = segmentId(segment).value_or(0);
          }
        }
      if (!at.empty())
        at += '/';
      if (past)
        at += kOldNodeIdPrefix;
      at += segment;
    }
    if (fromId && m_edits && pointer)
    {
      NodeIdEdit edit;
      edit.pointer = *pointer + where();
      edit.key = isKey;
      edit.from = path;
      edit.to = unusedPath(key, at);
      edit.model = std::move(model);
      edit.fromId = *fromId;
      edit.toId = toId;
      m_edits->push_back(std::move(edit));
    }
    return at;
  }

  // One warning naming what an instance's entry keeps unused.
  static void warnUnused(const std::string& prefab, const json& unused)
  {
    std::string list;
    const auto item = [&](const std::string& text)
    { list += (list.empty() ? "" : "; ") + text; };
    if (const auto it = unused.find("$overrides"); it != unused.end())
      for (const auto& [path, patch] : it->items())
      {
        std::string components;
        if (patch.is_object())
          for (const auto& [type, fields] : patch.items())
            components += (components.empty() ? "" : ", ") + type;
        item("overrides of " + path + " (" + components + ")");
      }
    if (const auto it = unused.find("$removed"); it != unused.end())
      for (const json& path : *it)
        item("removal of " + path.dump());
    if (const auto it = unused.find("$added"); it != unused.end())
      for (const json& extra : *it)
        item("an entity added under " + extra.value("$parent", json()).dump());
    LOG_WARN("Prefab " + prefab +
             ": an instance's saved changes name entities it no longer has (" +
             list + "); they're kept, unused, and saved back");
  }

  void add(Node node)
  {
    if (m_index.contains(node.key))
    {
      LOG_ERROR("Prefab entity " + node.key +
                " appears twice (duplicate $id); the copy is left out");
      return;
    }
    m_index[node.key] = nodes.size();
    nodes.push_back(std::move(node));
  }

  // A node and everything below it.
  void remove(const std::string& key)
  {
    const auto it = m_index.find(key);
    if (it == m_index.end())
      return;
    std::unordered_set<std::string> gone{key};
    nodes[it->second].removed = true;
    for (std::size_t i = it->second + 1; i < nodes.size(); ++i)
      if (gone.contains(nodes[i].parent))
      {
        nodes[i].removed = true;
        gone.insert(nodes[i].key);
      }
  }

  const SceneSerializer::PrefabSource& m_source;
  bool m_warn = false;
  std::vector<NodeIdEdit>* m_edits = nullptr;
  std::vector<std::string>* m_unreadable = nullptr;
  std::unordered_map<std::string, std::size_t> m_index;
  std::vector<std::string> m_stack;
  int m_added = 0;
};

Entity nearestInstance(Entity entity)
{
  for (; entity.isValid(); entity = hierarchy::parentOf(entity))
    if (entity.hasComponent<PrefabInstanceComponent>())
      return entity;
  return {};
}
} // namespace

Entity SceneSerializer::instanceRootOf(const Entity& entity)
{
  return nearestInstance(entity);
}

std::optional<std::string> SceneSerializer::pathInInstance(const Entity& entity,
                                                           const Entity& root)
{
  if (entity == root)
    return std::string();
  if (!entity.isValid() || !entity.hasComponent<PrefabLinkComponent>())
    return std::nullopt;
  // Its id is in the file of the instance it belongs to: the nearest instance
  // root above it (for a nested instance's root, the one containing it).
  const Entity owner = nearestInstance(hierarchy::parentOf(entity));
  if (!owner.isValid())
    return std::nullopt;
  const std::optional<std::string> ownerPath = pathInInstance(owner, root);
  if (!ownerPath)
    return std::nullopt;
  return join(*ownerPath,
              std::to_string(entity.getComponent<PrefabLinkComponent>().id));
}

Entity SceneSerializer::loadInstance(const json& entry,
                                     Scene& scene,
                                     const Entity& parent,
                                     std::set<std::string>* unresolvedTypes,
                                     std::vector<Entity>* created) const
{
  Expander expander(m_prefabs, true);
  expander.instance(entry, "", "", idOf(entry));

  std::unordered_map<std::string, Entity> made;
  Entity root;
  for (PrefabNode& node : expander.nodes)
  {
    if (node.removed)
      continue;
    Entity entity = scene.createEntity();
    const Entity above = node.key.empty() ? parent : made[node.parent];
    if (above.isValid())
      hierarchy::setParent(entity, above);
    made[node.key] = entity;
    if (created)
      created->push_back(entity);
    if (node.key.empty())
      root = entity;

    if (!node.missing.is_null())
    {
      json kept = node.missing;
      if (node.link)
        kept["$id"] = *node.link;
      entity.addComponent<MissingPrefabComponent>(
          MissingPrefabComponent{std::move(kept)});
      continue;
    }
    loadComponents(node.components, entity, unresolvedTypes);
    if (!node.unused.is_null())
    {
      if (!entity.hasComponent<UnresolvedComponents>())
        entity.addComponent<UnresolvedComponents>();
      entity.getComponent<UnresolvedComponents>().overrides =
          std::move(node.unused);
    }
    if (node.model)
      hierarchy::recordRestPose(entity);
    if (node.link)
      entity.addComponent<PrefabLinkComponent>(PrefabLinkComponent{*node.link});
    if (!node.prefab.empty())
      entity.addComponent<PrefabInstanceComponent>(
          PrefabInstanceComponent{node.prefab});
  }
  return root;
}

Entity SceneSerializer::instantiate(std::string_view prefab,
                                    Scene& scene,
                                    const Entity& parent,
                                    std::vector<Entity>* made) const
{
  return loadInstance(
      json{{"$prefab", std::string(prefab)}}, scene, parent, nullptr, made);
}

std::vector<SceneSerializer::PrefabNode>
SceneSerializer::expandBase(const json& entry) const
{
  Expander expander(m_prefabs);
  if (entry.is_object())
    expander.content(entry, "", "", std::nullopt, "");

  // Fill in every field of the components the registry knows, as saving an
  // entity writes them, so a field the prefab file lacks (added to the type
  // since) doesn't look like an override.
  for (PrefabNode& node : expander.nodes)
    for (auto& [typeName, fields] : node.components.items())
    {
      const reflect::TypeInfo* type = m_types.find(typeName);
      if (!type || type->kind != reflect::TypeKind::Component ||
          !fields.is_object())
        continue;
      auto [it, fresh] = m_defaults.try_emplace(typeName);
      if (fresh)
      {
        Registry scratch;
        Entity entity = scratch.createEntity();
        type->component.add(entity);
        it->second = saveObject(*type, type->component.get(entity));
      }
      json full = it->second;
      for (const auto& [field, value] : fields.items())
        if (full.contains(field))
          full[field] = value;
      fields = std::move(full);
    }
  return std::move(expander.nodes);
}

std::vector<SceneSerializer::ExpandedEntity>
SceneSerializer::expandPrefab(std::string_view prefab) const
{
  return expandEntry(json{{"$prefab", std::string(prefab)}});
}

std::vector<SceneSerializer::ExpandedEntity>
SceneSerializer::expandEntry(const json& entry) const
{
  std::vector<ExpandedEntity> entities;
  for (PrefabNode& node : expandBase(entry))
    if (!node.removed && node.missing.is_null())
      entities.push_back({std::move(node.key),
                          std::move(node.parent),
                          std::move(node.components)});
  return entities;
}

std::vector<SceneSerializer::NodeIdEdit>
SceneSerializer::nodeIdEdits(const json& file,
                             std::vector<std::string>* unreadable) const
{
  std::vector<NodeIdEdit> edits;
  if (!file.is_object())
    return edits;
  Expander expander(m_prefabs, true, &edits, unreadable);
  if (const auto root = file.find("root");
      root != file.end() && root->is_object())
  {
    const std::string pointer = "/root";
    expander.content(*root, "", "", std::nullopt, "", &pointer);
  }
  if (const auto entities = file.find("entities");
      entities != file.end() && entities->is_array())
    for (std::size_t i = 0; i < entities->size(); ++i)
      if ((*entities)[i].is_object())
      {
        // Keys of their own, apart from each other's and the root's.
        const std::string key = "+entity" + std::to_string(i);
        const std::string pointer = "/entities/" + std::to_string(i);
        expander.content((*entities)[i], key, "", std::nullopt, key, &pointer);
      }
  return edits;
}

json SceneSerializer::instanceEntry(const Entity& root) const
{
  const std::string prefab =
      root.getComponent<PrefabInstanceComponent>().prefab;
  const std::vector<PrefabNode> base = expandBase(json{{"$prefab", prefab}});
  std::unordered_map<std::string, const PrefabNode*> byPath;
  for (const PrefabNode& node : base)
    if (!node.removed && node.missing.is_null())
      byPath[node.key] = &node;

  json overrides = json::object();
  json added = json::array();
  std::unordered_set<std::string> seen;

  const std::function<void(const Entity&, const std::string&)> walk =
      [&](const Entity& entity, const std::string& parentPath)
  {
    const std::optional<std::string> path = pathInInstance(entity, root);
    const auto node = path ? byPath.find(*path) : byPath.end();
    const std::string isInstanceOf =
        entity.hasComponent<PrefabInstanceComponent>()
            ? entity.getComponent<PrefabInstanceComponent>().prefab
            : "";
    // Not the prefab's, moved to another parent in it, or now an instance of
    // another prefab (e.g. made into one since): saved whole.
    if (entity != root &&
        (node == byPath.end() || node->second->parent != parentPath ||
         node->second->prefab != isInstanceOf))
    {
      // What a prefab added here keeps its ids; one moved from elsewhere in
      // the prefab is a copy (the original is removed) and gets none.
      json extra = saveSubtree(
          entity, node == byPath.end() ? SavedIds::Added : SavedIds::None);
      extra["$parent"] = parentPath;
      added.push_back(std::move(extra));
      return;
    }
    if (node == byPath.end())
      return; // the root, with the prefab unreadable: nothing to compare with
    seen.insert(*path);
    // A nested instance whose prefab didn't load has nothing to compare.
    if (!entity.hasComponent<MissingPrefabComponent>())
    {
      json patch = mergeDiff(node->second->components, saveComponents(entity));
      if (!patch.empty())
        overrides[*path] = std::move(patch);
    }
    for (const Entity& child : hierarchy::childrenOf(entity))
      walk(child, *path);
  };
  walk(root, std::string());

  // The prefab's entities this one lacks (just the tops of removed branches).
  json removed = json::array();
  for (const PrefabNode& node : base)
    if (!node.removed && !node.key.empty() && !seen.contains(node.key) &&
        seen.contains(node.parent))
      removed.push_back(node.key);

  // What the loaded entry named that the prefab doesn't have goes back out,
  // each list item in the place it was read from.
  if (root.hasComponent<UnresolvedComponents>())
  {
    const json& unused = root.getComponent<UnresolvedComponents>().overrides;
    if (const auto it = unused.find("$overrides");
        it != unused.end() && it->is_object())
      for (const auto& [path, patch] : it->items())
        if (!overrides.contains(path))
          overrides[path] = patch;
    const auto restore = [&](json& list, const char* items, const char* places)
    {
      const auto kept = unused.find(items);
      if (kept == unused.end() || !kept->is_array())
        return;
      const auto at = unused.find(places);
      for (std::size_t k = 0; k < kept->size(); ++k)
      {
        std::size_t place = list.size();
        if (at != unused.end() && at->is_array() && k < at->size() &&
            (*at)[k].is_number_unsigned())
          place = std::min(place, (*at)[k].get<std::size_t>());
        list.insert(
            list.begin() + static_cast<std::ptrdiff_t>(place), (*kept)[k]);
      }
    };
    restore(removed, "$removed", "$removedAt");
    restore(added, "$added", "$addedAt");
  }

  json entry = {{"$prefab", prefab}};
  if (!overrides.empty())
    entry["$overrides"] = std::move(overrides);
  if (!removed.empty())
    entry["$removed"] = std::move(removed);
  if (!added.empty())
    entry["$added"] = std::move(added);
  return entry;
}

json SceneSerializer::savePrefab(const Entity& root,
                                 bool keepRootPlacement) const
{
  // Give the entities that will belong to the prefab ids: kept where they
  // have them (from the prefab they came from), new ones past the highest.
  // A nested instance's inside belongs to its own prefab, so isn't visited.
  uint32_t next = 1;
  std::vector<Entity> owned;
  const std::function<void(const Entity&)> collect = [&](const Entity& entity)
  {
    if (entity != root)
    {
      owned.push_back(entity);
      // A model node's stable id (from an unpacked model) or an added
      // entity's is kept, but new ids count on from the prefab's own.
      if (entity.hasComponent<PrefabLinkComponent>() &&
          entity.getComponent<PrefabLinkComponent>().id < kAddedNodeIdBit)
        next =
            std::max(next, entity.getComponent<PrefabLinkComponent>().id + 1);
      if (entity.hasComponent<PrefabInstanceComponent>() ||
          entity.hasComponent<MissingPrefabComponent>())
        return;
    }
    for (const Entity& child : hierarchy::childrenOf(entity))
      collect(child);
  };
  collect(root);
  std::unordered_set<uint32_t> used;
  for (Entity entity : owned)
  {
    // Kept only if it's unique here (an entity moved in from another prefab
    // may clash).
    if (entity.hasComponent<PrefabLinkComponent>() &&
        used.insert(entity.getComponent<PrefabLinkComponent>().id).second)
      continue;
    if (entity.hasComponent<PrefabLinkComponent>())
      entity.getComponent<PrefabLinkComponent>().id = next;
    else
      entity.addComponent<PrefabLinkComponent>(PrefabLinkComponent{next});
    used.insert(next++);
  }

  // What the prefab adds inside its nested instances belongs to it too. Each
  // new one gets an id (kAddedNodeIdBit) made from the id of the nested
  // instance whose "$added" saves it, unique under the root.
  std::unordered_set<uint32_t> addedIds;
  std::vector<std::pair<Entity, uint32_t>> fresh;
  const std::function<void(const Entity&, uint32_t)> inside =
      [&](const Entity& entity, uint32_t holder)
  {
    for (const Entity& child : hierarchy::childrenOf(entity))
    {
      if (child.hasComponent<MissingPrefabComponent>())
        continue;
      if (!child.hasComponent<PrefabLinkComponent>())
        fresh.emplace_back(child, holder);
      else if (const uint32_t id = child.getComponent<PrefabLinkComponent>().id;
               (id & kAddedNodeIdBit) != 0 && (id & kModelNodeIdBit) == 0)
        addedIds.insert(id);
      inside(child, holder);
    }
  };
  for (const Entity& entity : owned)
    if (entity.hasComponent<PrefabInstanceComponent>())
      inside(entity, entity.getComponent<PrefabLinkComponent>().id);
  for (auto& [entity, holder] : fresh)
  {
    uint32_t id = 0;
    for (std::size_t place = 0;
         addedIds.contains(id = addedNodeId(holder, '+', place));
         ++place)
    {
    }
    addedIds.insert(id);
    entity.addComponent<PrefabLinkComponent>(PrefabLinkComponent{id});
  }

  // The root's own content (not as an instance, even if it is one).
  json rootEntry = saveComponents(root);
  if (root.hasComponent<PrefabInstanceComponent>())
  {
    // Apply: the prefab keeps its own root placement.
    for (const PrefabNode& node : expandBase(json{
             {"$prefab", root.getComponent<PrefabInstanceComponent>().prefab}}))
      if (node.key.empty())
      {
        const auto transform = node.components.find("TransformComponent");
        if (transform != node.components.end())
          rootEntry["TransformComponent"] = *transform;
        else
          rootEntry.erase("TransformComponent");
      }
  }
  else if (rootEntry.contains("TransformComponent") && !keepRootPlacement)
  {
    // A new prefab is placed by its instances: its own root sits at the origin
    // (keeping its turn and size).
    rootEntry["TransformComponent"]["position"] = json::array({0.0, 0.0, 0.0});
  }

  json children = json::array();
  for (const Entity& child : hierarchy::childrenOf(root))
    children.push_back(saveSubtree(child, SavedIds::All));
  if (!children.empty())
    rootEntry[kChildrenKey] = std::move(children);
  return {
      {"formatVersion", kPrefabFormatVersion}, {"root", std::move(rootEntry)}};
}

} // namespace sfs
