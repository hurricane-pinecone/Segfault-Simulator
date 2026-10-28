// SceneSerializer's prefab side: expanding instances (with nesting and
// overrides) into entities, and saving an instance back as its prefab plus how
// it differs. See sceneSerializer.h for the format.

#include "engine/runtime/serialization/sceneSerializer.h"

#include "engine/core/ecs/registry.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/serialization/prefabComponents.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace sfs
{

using json = nlohmann::json;

namespace
{
constexpr const char* kChildrenKey = "children";
constexpr int kPrefabFormatVersion = 1;
constexpr int kMaxNesting = 16; // deeper than this is a prefab containing itself

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
  std::string key;    // its path in the instance ("+n" segments: not from the prefab)
  std::string parent; // its parent's key; the top node's is ignored
  json components = json::object();
  std::optional<uint32_t> link; // its id in the file that made it
  std::string prefab;           // set: the root of an instance of this
  json missing;                 // set: an instance whose prefab didn't load (its entry)
  bool removed = false;
};

namespace
{
// Expands instance entries into PrefabNodes, parents first.
class Expander
{
public:
  using Node = SceneSerializer::PrefabNode;

  explicit Expander(const SceneSerializer::PrefabSource& source) : m_source(source) {}

  std::vector<Node> nodes;

  void instance(const json& entry, const std::string& key, const std::string& parent, std::optional<uint32_t> link)
  {
    const std::string name = entry.value("$prefab", std::string());
    std::optional<json> data;
    const bool cycle = std::find(m_stack.begin(), m_stack.end(), name) != m_stack.end() ||
                       m_stack.size() >= kMaxNesting;
    if (cycle)
      LOG_ERROR("Prefab " + name + " contains itself; that copy is left out");
    else if (m_source)
      data = m_source(name);
    if (!data || !data->is_object() || !data->contains("root") || !(*data)["root"].is_object())
    {
      if (!cycle)
        LOG_ERROR("Can't read the prefab " + name + "; its instance is kept as saved");
      add({key, parent, json::object(), link, name, entry});
      return;
    }

    m_stack.push_back(name);
    content((*data)["root"], key, parent, link, key);
    nodes[m_index[key]].prefab = name;

    // How this instance differs, on top of the prefab (and its own nested
    // instances' differences, already applied).
    if (const auto overrides = entry.find("$overrides"); overrides != entry.end() && overrides->is_object())
      for (const auto& [path, patch] : overrides->items())
        if (const auto it = m_index.find(join(key, path)); it != m_index.end() && !nodes[it->second].removed)
          mergePatch(nodes[it->second].components, patch);
    if (const auto removed = entry.find("$removed"); removed != entry.end() && removed->is_array())
      for (const json& path : *removed)
        if (path.is_string())
          remove(join(key, path.get<std::string>()));
    if (const auto added = entry.find("$added"); added != entry.end() && added->is_array())
      for (const json& extra : *added)
      {
        const std::string parentKey = join(key, extra.value("$parent", std::string()));
        if (!extra.is_object() || !m_index.contains(parentKey) || nodes[m_index[parentKey]].removed)
          continue;
        const std::string extraKey = join(key, "+" + std::to_string(m_added++));
        if (extra.contains("$prefab"))
          instance(extra, extraKey, parentKey, std::nullopt);
        else
          content(extra, extraKey, parentKey, std::nullopt, extraKey);
      }
    m_stack.pop_back();
  }

  // A prefab's (or scene's) entry: entities with "$id"s are keyed by them under
  // `owner` (the instance they belong to); others get "+n" keys.
  void content(const json& entry,
               const std::string& key,
               const std::string& parent,
               std::optional<uint32_t> link,
               const std::string& owner)
  {
    if (entry.contains("$prefab"))
    {
      instance(entry, key, parent, link);
      return;
    }
    add({key, parent, componentsOf(entry), link});
    if (const auto children = entry.find(kChildrenKey); children != entry.end() && children->is_array())
      for (const json& child : *children)
      {
        if (!child.is_object())
          continue;
        const std::optional<uint32_t> childId = idOf(child);
        const std::string childKey =
            childId ? join(owner, std::to_string(*childId)) : join(key, "+" + std::to_string(m_added++));
        content(child, childKey, key, childId, childId ? owner : childKey);
      }
  }

private:
  void add(Node node)
  {
    if (m_index.contains(node.key))
    {
      LOG_ERROR("Prefab entity " + node.key + " appears twice (duplicate $id); the copy is left out");
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

std::optional<std::string> SceneSerializer::pathInInstance(const Entity& entity, const Entity& root)
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
  return join(*ownerPath, std::to_string(entity.getComponent<PrefabLinkComponent>().id));
}

Entity SceneSerializer::loadInstance(const json& entry,
                                     Scene& scene,
                                     const Entity& parent,
                                     std::set<std::string>* unresolvedTypes) const
{
  Expander expander(m_prefabs);
  expander.instance(entry, "", "", idOf(entry));

  std::unordered_map<std::string, Entity> made;
  Entity root;
  for (const PrefabNode& node : expander.nodes)
  {
    if (node.removed)
      continue;
    Entity entity = scene.createEntity();
    const Entity above = node.key.empty() ? parent : made[node.parent];
    if (above.isValid())
      hierarchy::setParent(entity, above);
    made[node.key] = entity;
    if (node.key.empty())
      root = entity;

    if (!node.missing.is_null())
    {
      json kept = node.missing;
      if (node.link)
        kept["$id"] = *node.link;
      entity.addComponent<MissingPrefabComponent>(MissingPrefabComponent{std::move(kept)});
      continue;
    }
    loadComponents(node.components, entity, unresolvedTypes);
    if (node.link)
      entity.addComponent<PrefabLinkComponent>(PrefabLinkComponent{*node.link});
    if (!node.prefab.empty())
      entity.addComponent<PrefabInstanceComponent>(PrefabInstanceComponent{node.prefab});
  }
  return root;
}

Entity SceneSerializer::instantiate(std::string_view prefab, Scene& scene, const Entity& parent) const
{
  return loadInstance(json{{"$prefab", std::string(prefab)}}, scene, parent, nullptr);
}

std::vector<SceneSerializer::PrefabNode> SceneSerializer::expandBase(std::string_view prefab) const
{
  Expander expander(m_prefabs);
  expander.instance(json{{"$prefab", std::string(prefab)}}, "", "", std::nullopt);

  // Fill in every field of the components the registry knows, as saving an
  // entity writes them, so a field the prefab file lacks (added to the type
  // since) doesn't look like an override.
  for (PrefabNode& node : expander.nodes)
    for (auto& [typeName, fields] : node.components.items())
    {
      const reflect::TypeInfo* type = m_types.find(typeName);
      if (!type || type->kind != reflect::TypeKind::Component || !fields.is_object())
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

std::vector<SceneSerializer::ExpandedEntity> SceneSerializer::expandPrefab(std::string_view prefab) const
{
  std::vector<ExpandedEntity> entities;
  for (PrefabNode& node : expandBase(prefab))
    if (!node.removed && node.missing.is_null())
      entities.push_back({std::move(node.key), std::move(node.parent), std::move(node.components)});
  return entities;
}

json SceneSerializer::instanceEntry(Scene& scene, const Entity& root) const
{
  return instanceEntry(root, hierarchy::EntityTree(scene.entities()));
}

json SceneSerializer::instanceEntry(const Entity& root, const hierarchy::EntityTree& tree) const
{
  const std::string prefab = root.getComponent<PrefabInstanceComponent>().prefab;
  const std::vector<PrefabNode> base = expandBase(prefab);
  std::unordered_map<std::string, const PrefabNode*> byPath;
  for (const PrefabNode& node : base)
    if (!node.removed && node.missing.is_null())
      byPath[node.key] = &node;

  json overrides = json::object();
  json added = json::array();
  std::unordered_set<std::string> seen;

  const std::function<void(const Entity&, const std::string&)> walk = [&](const Entity& entity,
                                                                         const std::string& parentPath)
  {
    const std::optional<std::string> path = pathInInstance(entity, root);
    const auto node = path ? byPath.find(*path) : byPath.end();
    const std::string isInstanceOf =
        entity.hasComponent<PrefabInstanceComponent>() ? entity.getComponent<PrefabInstanceComponent>().prefab : "";
    // Not the prefab's, moved to another parent in it, or now an instance of
    // another prefab (e.g. made into one since): saved whole.
    if (entity != root && (node == byPath.end() || node->second->parent != parentPath ||
                           node->second->prefab != isInstanceOf))
    {
      json extra = saveSubtree(entity, tree);
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
    for (const Entity& child : tree.children(entity))
      walk(child, *path);
  };
  walk(root, std::string());

  // The prefab's entities this one lacks (just the tops of removed branches).
  json removed = json::array();
  for (const PrefabNode& node : base)
    if (!node.removed && !node.key.empty() && !seen.contains(node.key) && seen.contains(node.parent))
      removed.push_back(node.key);

  json entry = {{"$prefab", prefab}};
  if (!overrides.empty())
    entry["$overrides"] = std::move(overrides);
  if (!removed.empty())
    entry["$removed"] = std::move(removed);
  if (!added.empty())
    entry["$added"] = std::move(added);
  return entry;
}

json SceneSerializer::savePrefab(Scene& scene, const Entity& root, bool keepRootPlacement) const
{
  const hierarchy::EntityTree tree(scene.entities());

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
      if (entity.hasComponent<PrefabLinkComponent>())
        next = std::max(next, entity.getComponent<PrefabLinkComponent>().id + 1);
      if (entity.hasComponent<PrefabInstanceComponent>() || entity.hasComponent<MissingPrefabComponent>())
        return;
    }
    for (const Entity& child : tree.children(entity))
      collect(child);
  };
  collect(root);
  std::unordered_set<uint32_t> used;
  for (Entity entity : owned)
  {
    // Kept only if it's unique here (an entity moved in from another prefab
    // may clash).
    if (entity.hasComponent<PrefabLinkComponent>() && used.insert(entity.getComponent<PrefabLinkComponent>().id).second)
      continue;
    if (entity.hasComponent<PrefabLinkComponent>())
      entity.getComponent<PrefabLinkComponent>().id = next;
    else
      entity.addComponent<PrefabLinkComponent>(PrefabLinkComponent{next});
    used.insert(next++);
  }

  // The root's own content (not as an instance, even if it is one).
  json rootEntry = saveComponents(root);
  if (root.hasComponent<PrefabInstanceComponent>())
  {
    // Apply: the prefab keeps its own root placement.
    for (const PrefabNode& node : expandBase(root.getComponent<PrefabInstanceComponent>().prefab))
      if (node.key.empty())
      {
        const auto transform = node.components.find("Transform3DComponent");
        if (transform != node.components.end())
          rootEntry["Transform3DComponent"] = *transform;
        else
          rootEntry.erase("Transform3DComponent");
      }
  }
  else if (rootEntry.contains("Transform3DComponent") && !keepRootPlacement)
  {
    // A new prefab is placed by its instances: its own root sits at the origin
    // (keeping its turn and size).
    rootEntry["Transform3DComponent"]["position"] = json::array({0.0, 0.0, 0.0});
  }

  json children = json::array();
  for (const Entity& child : tree.children(root))
    children.push_back(saveSubtree(child, tree, true));
  if (!children.empty())
    rootEntry[kChildrenKey] = std::move(children);
  return {{"formatVersion", kPrefabFormatVersion}, {"root", std::move(rootEntry)}};
}

} // namespace sfs
