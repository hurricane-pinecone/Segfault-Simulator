#include "scene/unusedOverrides.h"

#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/runtime/assetStore/modelData.h"
#include "engine/runtime/assetStore/modelImporter.h"
#include "engine/runtime/serialization/prefabComponents.h"
#include "engine/runtime/serialization/sceneSerializer.h"
#include "engine/runtime/serialization/unresolvedComponents.h"

#include <algorithm>
#include <optional>
#include <set>

namespace unusedOverrides
{

using json = nlohmann::json;

namespace
{
// An override's patch merged into another's: objects merge, the rest of
// `patch` (null included: a component the instance removes) replaces.
void mergeOverride(json& into, const json& patch)
{
  if (!into.is_object() || !patch.is_object())
  {
    into = patch;
    return;
  }
  for (const auto& [key, value] : patch.items())
    if (value.is_object() && into.contains(key) && into[key].is_object())
      mergeOverride(into[key], value);
    else
      into[key] = value;
}
} // namespace

std::vector<Unused> of(const sfs::Entity& root)
{
  std::vector<Unused> out;
  sfs::UnresolvedComponents* unresolvedComponents =
      root.getComponent<sfs::UnresolvedComponents>();
  if (!unresolvedComponents)
    return out;
  const json& unused = unresolvedComponents->overrides;
  if (!unused.is_object())
    return out;
  if (const auto it = unused.find("$overrides");
      it != unused.end() && it->is_object())
    for (const auto& [path, patch] : it->items())
      out.push_back({Unused::Kind::Override, path, patch});
  if (const auto it = unused.find("$removed");
      it != unused.end() && it->is_array())
    for (const json& path : *it)
      if (path.is_string())
        out.push_back(
            {Unused::Kind::Removed, path.get<std::string>(), nullptr});
  if (const auto it = unused.find("$added");
      it != unused.end() && it->is_array())
    for (const json& extra : *it)
      if (extra.is_object())
        out.push_back({Unused::Kind::Added,
                       extra.value("$parent", std::string()),
                       extra});
  return out;
}

void move(json& entry, const std::map<std::string, std::string>& moves)
{
  if (const auto overrides = entry.find("$overrides");
      overrides != entry.end() && overrides->is_object())
    for (const auto& [from, to] : moves)
      if (const auto it = overrides->find(from); it != overrides->end())
      {
        json patch = std::move(*it);
        overrides->erase(from);
        mergeOverride((*overrides)[to], patch);
      }
  if (const auto removed = entry.find("$removed");
      removed != entry.end() && removed->is_array())
    for (json& path : *removed)
      if (path.is_string())
        if (const auto it = moves.find(path.get<std::string>());
            it != moves.end())
          path = it->second;
  if (const auto added = entry.find("$added");
      added != entry.end() && added->is_array())
    for (json& extra : *added)
      if (const auto parent = extra.find("$parent");
          parent != extra.end() && parent->is_string())
        if (const auto it = moves.find(parent->get<std::string>());
            it != moves.end())
          *parent = it->second;
}

void remove(json& entry, const Unused& item)
{
  const char* list = item.kind == Unused::Kind::Override  ? "$overrides"
                     : item.kind == Unused::Kind::Removed ? "$removed"
                                                          : "$added";
  const auto it = entry.find(list);
  if (it == entry.end())
    return;
  if (item.kind == Unused::Kind::Override)
    it->erase(item.path);
  else if (it->is_array())
  {
    const json match =
        item.kind == Unused::Kind::Removed ? json(item.path) : item.value;
    if (const auto found = std::find(it->begin(), it->end(), match);
        found != it->end())
      it->erase(found);
  }
  if (it->empty())
    entry.erase(it);
}

std::map<std::string, std::string>
renamedNodeMoves(const sfs::Entity& root,
                 const sfs::Entity& node,
                 std::string_view oldName,
                 const std::vector<sfs::ModelNode>& nodes,
                 const std::vector<std::string>& paths)
{
  std::map<std::string, std::string> moves;
  sfs::PrefabLinkComponent* prefabLink =
      node.getComponent<sfs::PrefabLinkComponent>();
  if (!prefabLink || nodes.size() != paths.size())
    return moves;
  const uint32_t id = prefabLink->id;
  const auto renamed =
      std::find_if(nodes.begin(),
                   nodes.end(),
                   [&](const sfs::ModelNode& each) { return each.id == id; });
  const std::optional<std::string> nodePath =
      sfs::SceneSerializer::pathInInstance(node, root);
  if (renamed == nodes.end() || !nodePath)
    return moves;
  // The model instance's path, which every one of its nodes' paths starts
  // with: a model's nodes are all named by one id under it.
  const std::size_t slash = nodePath->rfind('/');
  const std::string model = slash == std::string::npos
                                ? std::string()
                                : nodePath->substr(0, slash + 1);

  // Its name path now, and as it was: its parent's, then its old name (its
  // own new name may hold an escaped "/", so it's not split off).
  const std::size_t index = static_cast<std::size_t>(renamed - nodes.begin());
  const std::string& now = paths[index];
  const int parent = renamed->parent;
  const std::string before =
      (parent >= 0 && static_cast<std::size_t>(parent) < paths.size()
           ? paths[static_cast<std::size_t>(parent)] + "/"
           : std::string()) +
      sfs::nodeNameSegment(oldName);

  std::set<std::string> unused;
  for (const Unused& item : of(root))
    unused.insert(item.path);
  for (std::size_t i = 0; i < nodes.size(); ++i)
  {
    const std::string& path = paths[i];
    if (path != now && !(path.size() > now.size() && path.starts_with(now) &&
                         path[now.size()] == '/'))
      continue;
    const std::string from =
        model +
        std::to_string(sfs::stableNodeId(before + path.substr(now.size())));
    if (unused.contains(from))
      moves[from] = model + std::to_string(nodes[i].id);
  }
  return moves;
}

} // namespace unusedOverrides
