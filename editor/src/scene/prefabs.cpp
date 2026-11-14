#include "scene/prefabs.h"

#include "editorContext.h"
#include "history/editorHistory.h"
#include "scene/editorScene.h"
#include "scene/parts.h"
#include "scene/references.h"
#include "scene/sceneDocument.h"
#include "scene/unusedOverrides.h"

#include "engine/core/animation/animation.h"
#include "engine/core/components/animatorComponent.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/modelData.h"
#include "engine/runtime/serialization/prefabComponents.h"
#include "engine/runtime/serialization/sceneSerializer.h"

#include <cctype>
#include <system_error>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace prefabs
{

namespace
{
std::string safeName(std::string name)
{
  for (char& c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '-' && c != '_')
      c = '_';
  return name.empty() ? "Prefab" : name;
}

// Replace an instance with one made from `entry` (same parent; the registry
// reuses the freed ids, so it keeps its place among its siblings), keeping its
// id in the prefab around it if it's a nested one. References other entities
// hold into it are pointed at the new entities. Called within an undoable
// change, whose snapshot has given each entity a reference names from outside
// its instance a scene id, so it's found again by that.
sfs::Entity rebuild(EditorContext& context, const sfs::Entity& root, json entry)
{
  const std::vector<references::Inbound> inbound = references::inboundTo(root);
  const sfs::Entity parent = sfs::hierarchy::parentOf(root);
  if (sfs::PrefabLinkComponent* prefabLink = root.getComponent<sfs::PrefabLinkComponent>())
    entry["$id"] = prefabLink->id;
  context.scene->destroyBranch(root);
  context.scene->flushEntities();
  const sfs::Entity made = context.scene->serializer().loadEntity(entry, *context.scene, parent);
  context.scene->flushEntities();
  references::restore(inbound, made, *context.scene);
  return made;
}

// The instance whose entry holds an entity's overrides, and its path in it.
std::optional<std::pair<sfs::Entity, std::string>> outermost(const sfs::Entity& entity)
{
  const sfs::Entity root = sfs::SceneSerializer::entryRootOf(entity);
  if (!root.isValid())
    return std::nullopt;
  const std::optional<std::string> path = sfs::SceneSerializer::pathInInstance(entity, root);
  if (!path)
    return std::nullopt; // added to the instance: its own, not overriding
  return std::make_pair(root, *path);
}
} // namespace

std::string nameOf(const sfs::AssetId& id)
{
  return "asset:" + id.toString();
}

const sfs::AssetRecord* recordOf(const EditorContext& context, const std::string& name)
{
  constexpr std::string_view prefix = "asset:";
  if (!context.assets || !name.starts_with(prefix))
    return nullptr;
  const std::optional<sfs::AssetId> id = sfs::AssetId::parse(std::string_view(name).substr(prefix.size()));
  return id ? context.assets->find(*id) : nullptr;
}

// The clip a newly placed animated model starts on: its "idle" (any case), or
// else its first; empty if it has none.
std::string startingClip(const sfs::AnimationSet& animations)
{
  for (const sfs::AnimationClip& clip : animations.clips)
  {
    std::string lower = clip.name;
    for (char& c : lower)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "idle")
      return clip.name;
  }
  return animations.clips.empty() ? std::string() : animations.clips.front().name;
}

std::string label(const EditorContext& context, const std::string& name)
{
  const sfs::AssetRecord* record = recordOf(context, name);
  return record ? record->path.generic_string() : name;
}

std::optional<sfs::AssetId> create(EditorContext& context, sfs::Entity entity, const fs::path& folder)
{
  if (!context.assets || !context.scene || !entity.isValid())
    return std::nullopt;

  std::error_code error;
  fs::create_directories(context.assets->root() / folder, error);
  const sfs::NameComponent* named = entity.getComponent<sfs::NameComponent>();
  const std::string stem = safeName(named ? named->name : std::string("Prefab"));
  fs::path relative = folder / (stem + ".prefab");
  for (int n = 2; fs::exists(context.assets->root() / relative, error); ++n)
    relative = folder / (stem + " " + std::to_string(n) + ".prefab");

  std::optional<sfs::AssetId> id;
  context.history->change(
      [&]
      {
        // Saving gives the entities their prefab ids; then it's an instance.
        const json prefab = context.scene->serializer().savePrefab(entity);
        if (!sfs::SceneSerializer::writeJsonFile(prefab, context.assets->root() / relative))
          return;
        context.assets->refresh();
        const sfs::AssetRecord* record = context.assets->findByPath(relative);
        if (!record)
          return;
        id = record->id;
        if (sfs::PrefabInstanceComponent* prefabInstance = entity.getComponent<sfs::PrefabInstanceComponent>())
          prefabInstance->prefab = nameOf(record->id);
        else
          entity.addComponent<sfs::PrefabInstanceComponent>(sfs::PrefabInstanceComponent{nameOf(record->id)});
      });
  if (id)
    LOG_INFO("Made the prefab " + relative.generic_string());
  return id;
}

sfs::Entity place(EditorContext& context,
                  const std::string& prefab,
                  const sfs::Entity& parent,
                  std::optional<glm::vec3> position)
{
  if (!context.scene)
    return {};
  sfs::Entity root;
  context.history->change(
      [&]
      {
        root = context.scene->serializer().instantiate(prefab, *context.scene, parent);
        context.scene->flushEntities();
        // An animated model comes with an Animator, playing its idle (or first)
        // clip, as a model with clips is placed to be animated.
        if (const sfs::AssetRecord* record = recordOf(context, prefab);
            root.isValid() && record && record->kind == sfs::AssetKind::Model &&
            !root.hasComponent<sfs::AnimatorComponent>())
          if (const sfs::ModelData* model = context.scene->assetStore().model(record->id);
              model && model->animations && !model->animations->clips.empty())
          {
            sfs::AnimatorComponent animator;
            animator.clip = startingClip(*model->animations);
            root.addComponent<sfs::AnimatorComponent>(animator);
          }
        if (!root.isValid() || !position)
          return;
        if (!root.hasComponent<sfs::TransformComponent>())
          root.addComponent<sfs::TransformComponent>();
        // The point is in the world; under a parent, place it there.
        glm::mat4 world = sfs::hierarchy::worldMatrix(root);
        world[3] = glm::vec4(*position, 1.0f);
        sfs::hierarchy::setWorldMatrix(root, world);
      });
  if (root.isValid())
  {
    context.selection = root;
    context.selectedAsset = {};
  }
  return root;
}

void apply(EditorContext& context, const sfs::Entity& root)
{
  sfs::PrefabInstanceComponent* prefabInstance = root.getComponent<sfs::PrefabInstanceComponent>();
  if (!prefabInstance || !context.scene)
    return;
  const std::string name = prefabInstance->prefab;
  const sfs::AssetRecord* record = recordOf(context, name);
  if (!record || record->kind != sfs::AssetKind::Prefab)
  {
    LOG_ERROR("Only a .prefab can take changes (" + label(context, name) + " isn't one)");
    return;
  }

  context.history->change(
      [&]
      {
        parts::heal(root, record->path.generic_string());
        // Every instance as it is now, against the prefab as it was: reloaded
        // against the new prefab, each keeps its own changes and takes the
        // applied ones (this one's become the prefab's, so no longer differ).
        const json prefab = context.scene->serializer().savePrefab(root);
        const auto write = [&]
        {
          if (!sfs::SceneSerializer::writeJsonFile(prefab, context.assets->absolutePath(*record)))
            return false;
          context.scene->assetStore().forgetPrefab(record->id);
          return true;
        };
        if (context.rewritePrefab)
          context.rewritePrefab(record->id, write);
        else
        {
          const json scene = context.document->snapshot();
          if (write())
            context.document->restore(scene);
        }
      });
  context.selection = {};
  LOG_INFO("Applied the changes to " + record->path.generic_string());
}

void revert(EditorContext& context, const sfs::Entity& root)
{
  if (!root.isValid() || !root.hasComponent<sfs::PrefabInstanceComponent>() || !context.scene)
    return;
  context.history->change(
      [&]
      {
        json entry = context.scene->serializer().instanceEntry(root);
        // Keep where it is: its root's transform, nothing else.
        json kept = json::object();
        if (entry.contains("$overrides") && entry["$overrides"].contains("") &&
            entry["$overrides"][""].contains("TransformComponent"))
          kept[""]["TransformComponent"] = entry["$overrides"][""]["TransformComponent"];
        entry.erase("$removed");
        entry.erase("$added");
        entry.erase("$overrides");
        if (!kept.empty())
          entry["$overrides"] = kept;
        context.selection = rebuild(context, root, std::move(entry));
      });
}

void revertField(EditorContext& context,
                 const sfs::Entity& entity,
                 const std::string& component,
                 const std::string& field)
{
  const auto where = outermost(entity);
  if (!where)
    return;
  const auto& [root, path] = *where;
  context.history->change(
      [&]
      {
        json entry = context.scene->serializer().instanceEntry(root);
        json& overrides = entry["$overrides"];
        if (!overrides.contains(path) || !overrides[path].contains(component))
          return;
        if (field.empty())
          overrides[path].erase(component);
        else
          overrides[path][component].erase(field);
        if (overrides[path].contains(component) && overrides[path][component].empty())
          overrides[path].erase(component);
        if (overrides[path].empty())
          overrides.erase(path);
        if (overrides.empty())
          entry.erase("$overrides");
        const sfs::Entity rebuilt = rebuild(context, root, std::move(entry));
        // Select the same entity in the rebuilt instance.
        context.selection = rebuilt;
        for (const sfs::Entity& candidate : context.scene->entities())
          if (sfs::SceneSerializer::pathInInstance(candidate, rebuilt) == path)
            context.selection = candidate;
      });
}

std::map<std::string, std::set<std::string>> overridesOf(EditorContext& context, const sfs::Entity& entity)
{
  std::map<std::string, std::set<std::string>> out;
  const auto where = outermost(entity);
  if (!where || !context.scene)
    return out;
  const auto& [root, path] = *where;
  const json entry = context.scene->serializer().instanceEntry(root);
  const auto overrides = entry.find("$overrides");
  if (overrides == entry.end() || !overrides->contains(path))
    return out;
  for (const auto& [component, fields] : (*overrides)[path].items())
  {
    std::set<std::string>& names = out[component];
    if (fields.is_object())
      for (const auto& [name, value] : fields.items())
        names.insert(name);
  }
  return out;
}

void reattachUnused(EditorContext& context, const sfs::Entity& root, const std::map<std::string, std::string>& moves)
{
  if (moves.empty() || !context.scene || !root.isValid() || !root.hasComponent<sfs::PrefabInstanceComponent>())
    return;
  context.history->change(
      [&]
      {
        json entry = context.scene->serializer().instanceEntry(root);
        unusedOverrides::move(entry, moves);
        context.selection = rebuild(context, root, std::move(entry));
      });
}

void removeUnused(EditorContext& context, const sfs::Entity& root, const unusedOverrides::Unused& item)
{
  if (!context.scene || !root.isValid() || !root.hasComponent<sfs::PrefabInstanceComponent>())
    return;
  context.history->change(
      [&]
      {
        json entry = context.scene->serializer().instanceEntry(root);
        unusedOverrides::remove(entry, item);
        context.selection = rebuild(context, root, std::move(entry));
      });
}

} // namespace prefabs
