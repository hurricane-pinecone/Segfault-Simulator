#include "build/fillParts.h"

#include "scene/parts.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/core/util/atomicFile.h"
#include "engine/core/util/jsonTokens.h"
#include "engine/core/util/readFileText.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/modelData.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/serialization/legacyComponentNames.h"
#include "engine/runtime/serialization/prefabComponents.h"
#include "engine/runtime/serialization/sceneSerializer.h"

#include <algorithm>
#include <exception>
#include <optional>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
// A value the fill changed, by its JSON pointer in the prefab's file.
struct Change
{
  std::string pointer;
  const json* value = nullptr;
};

// Every value of `after` that differs from `before`: members of objects both
// have are looked into; any other difference is the whole value.
void changesOf(const json& before,
               const json& after,
               const std::string& pointer,
               std::vector<Change>& out)
{
  if (!before.is_object() || !after.is_object())
  {
    if (before != after)
      out.push_back({pointer, &after});
    return;
  }
  for (const auto& [key, value] : after.items())
  {
    const std::string at = pointer + "/" + sfs::jsonPointerSegment(key);
    if (const auto it = before.find(key); it != before.end())
      changesOf(*it, value, at, out);
    else
      out.push_back({at, &value});
  }
}

// How deep the value at a pointer sits (the document is 0).
int depthOf(std::string_view pointer)
{
  return static_cast<int>(std::count(pointer.begin(), pointer.end(), '/'));
}

std::string parentOf(std::string_view pointer)
{
  const std::size_t slash = pointer.rfind('/');
  return std::string(
      pointer.substr(0, slash == std::string_view::npos ? 0 : slash));
}

std::string indent(int depth)
{
  return std::string(static_cast<std::size_t>(std::max(depth, 0)) * 2, ' ');
}

// The file's text with each changed value replaced, in the editor's style
// (sfs::SceneSerializer::jsonText()), and every other byte as it was. A value
// the file lacks is inserted into the nearest object it has, in key order, as
// the editor writes keys. Nothing (logged) if that can't be done.
std::optional<std::string> editInPlace(const std::string& text,
                                       const json& after,
                                       const std::vector<Change>& changes,
                                       const std::string& path)
{
  const std::optional<std::vector<sfs::JsonToken>> tokens =
      sfs::scanJsonTokens(text);
  if (!tokens)
  {
    LOG_ERROR(path + ": not valid JSON");
    return std::nullopt;
  }
  std::vector<sfs::TextEdit> edits;
  std::vector<std::string> whole; // pointers written whole: what's below is in
  const auto within = [&](const std::string& pointer)
  {
    return std::any_of(whole.begin(),
                       whole.end(),
                       [&](const std::string& done)
                       { return pointer.starts_with(done + "/"); });
  };
  for (const Change& change : changes)
  {
    if (within(change.pointer))
      continue;
    if (const std::optional<sfs::JsonSpan> span =
            sfs::findJsonValue(text, change.pointer))
    {
      edits.push_back({span->begin,
                       span->end,
                       sfs::SceneSerializer::jsonText(
                           *change.value, depthOf(change.pointer))});
      continue;
    }

    // Not in the file: the member under the nearest object it has goes in.
    std::string at = change.pointer;
    std::optional<sfs::JsonSpan> parent;
    while (!at.empty() && !(parent = sfs::findJsonValue(text, parentOf(at))))
      at = parentOf(at);
    const std::string into = parentOf(at);
    if (at.empty() || !parent || text[parent->begin] != '{')
    {
      LOG_ERROR(path + ": has no place for " + change.pointer);
      return std::nullopt;
    }
    whole.push_back(at);
    const int depth = depthOf(at);
    const std::string key = json::json_pointer(at).back();
    const std::string member =
        json(key).dump() + ": " +
        sfs::SceneSerializer::jsonText(after.at(json::json_pointer(at)), depth);

    // Its place among the object's members: before the first that sorts
    // after it, else after the last.
    const sfs::JsonToken* before = nullptr;
    const sfs::JsonToken* last = nullptr;
    for (const sfs::JsonToken& token : *tokens)
      if (token.kind == sfs::JsonToken::Kind::Key &&
          parentOf(token.pointer) == into)
      {
        last = &token;
        if (!before && token.text > key)
          before = &token;
      }
    if (!last)
      edits.push_back(
          {parent->begin,
           parent->end,
           "{\n" + indent(depth) + member + "\n" + indent(depth - 1) + "}"});
    else if (before)
      edits.push_back(
          {before->begin, before->begin, member + ",\n" + indent(depth)});
    else if (const std::optional<sfs::JsonSpan> value =
                 sfs::findJsonValue(text, last->pointer))
      edits.push_back({value->end, value->end, ",\n" + indent(depth) + member});
  }

  // Members going in at one place go in in key order, as one edit.
  std::vector<sfs::TextEdit> merged;
  for (sfs::TextEdit& edit : edits)
    if (!merged.empty() && edit.begin == edit.end &&
        merged.back().begin == edit.begin && merged.back().end == edit.end)
      merged.back().text += edit.text;
    else
      merged.push_back(std::move(edit));
  std::optional<std::string> edited = sfs::applyTextEdits(text, merged);
  if (!edited)
    LOG_ERROR(path + ": its edits overlap");
  return edited;
}

FillPartsReport
fill(const fs::path& project, std::string_view which, bool write)
{
  FillPartsReport report;
  std::error_code error;
  if (!fs::is_directory(project, error))
  {
    LOG_ERROR("No project folder at " + project.string());
    return report;
  }
  const sfs::AssetDatabase database(project,
                                    sfs::AssetDatabase::Mode::ReadOnly,
                                    sfs::AssetDatabase::projectFolders());
  sfs::AssetStore store;
  store.setDatabase(&database);
  sfs::Scene scene{0, sfs::SceneServices{.assetStore = store}};
  // A prefab is read from its edited text while that's checked.
  std::string checking;
  std::optional<json> edited;
  // No mesh library: mesh fields load empty, which the part values, taken
  // from the save, never involve (and the file is only edited, not saved).
  sfs::SceneSerializer serializer(sfs::reflect::types(), nullptr);
  serializer.setPrefabSource(
      [&](std::string_view name) -> std::optional<json>
      {
        if (edited && name == checking)
          return edited;
        const std::optional<sfs::AssetMeshName> asset =
            sfs::parseAssetMeshName(name);
        return asset && asset->part.empty() ? store.prefab(asset->model)
                                            : std::nullopt;
      });
  const auto clear = [&]
  {
    for (const sfs::Entity& entity : scene.entities())
      scene.destroyEntity(entity);
    scene.flushEntities();
  };
  // As the editor's prefab tab opens one: its root no longer an instance.
  const auto open = [&](const std::string& name)
  {
    clear();
    sfs::Entity root = serializer.instantiate(name, scene);
    scene.flushEntities();
    if (root.isValid() && root.hasComponent<sfs::PrefabInstanceComponent>())
      root.removeComponent<sfs::PrefabInstanceComponent>();
    else
      root = {};
    return root;
  };
  // A nested instance of another .prefab: that prefab's own file holds its
  // part fields.
  const auto nestedPrefab = [&](const sfs::Entity& entity)
  {
    sfs::PrefabInstanceComponent* prefabInstance =
        entity.getComponent<sfs::PrefabInstanceComponent>();
    if (!prefabInstance)
      return false;
    const std::optional<sfs::AssetMeshName> asset =
        sfs::parseAssetMeshName(prefabInstance->prefab);
    const sfs::AssetRecord* record =
        asset ? database.find(asset->model) : nullptr;
    return record && record->kind == sfs::AssetKind::Prefab;
  };

  std::vector<const sfs::AssetRecord*> prefabs;
  for (const sfs::AssetRecord& record : database.records())
    if (record.kind == sfs::AssetKind::Prefab &&
        (which == "all" ||
         record.path.generic_string() ==
             fs::path(which).lexically_normal().generic_string()))
      prefabs.push_back(&record);
  std::sort(prefabs.begin(),
            prefabs.end(),
            [](const sfs::AssetRecord* a, const sfs::AssetRecord* b)
            { return a->path < b->path; });
  if (prefabs.empty())
  {
    LOG_ERROR(which == "all" ? std::string("The project has no prefabs")
                             : "No prefab at " + std::string(which));
    return report;
  }

  bool ok = true;
  for (const sfs::AssetRecord* record : prefabs)
  {
    ++report.prefabs;
    const std::string path = record->path.generic_string();
    const std::string name = "asset:" + record->id.toString();
    const fs::path file = database.absolutePath(*record);
    const std::optional<std::string> text = sfs::readFileText(file);
    const json read = text ? json::parse(*text, nullptr, false) : json();
    const sfs::Entity root = open(name);
    if (!text || read.is_discarded() || !root.isValid())
    {
      LOG_ERROR(path + ": can't be read");
      ok = false;
      continue;
    }

    const json before = serializer.savePrefab(root, true);
    const std::size_t fields = parts::fillAll(root, nestedPrefab);
    if (fields == 0)
      continue;
    const json after = serializer.savePrefab(root, true);
    report.changed.push_back({path, fields});

    // Edited in place: only the changed values' text. A file with old
    // component names would get the new ones beside them.
    json upgraded = read;
    sfs::upgradeComponentNames(upgraded);
    if (upgraded != read)
    {
      LOG_ERROR(path + ": uses old component names, so it can't be edited in "
                       "place: open and apply it in the editor first");
      ok = false;
      continue;
    }
    std::vector<Change> changes;
    changesOf(before, after, "", changes);
    const std::optional<std::string> result =
        editInPlace(*text, after, changes, path);
    // Checked: the edited file opens to what was filled.
    edited = result ? json::parse(*result, nullptr, false) : json();
    checking = name;
    const sfs::Entity again =
        result && !edited->is_discarded() ? open(name) : sfs::Entity();
    const bool same =
        again.isValid() && serializer.savePrefab(again, true) == after;
    edited.reset();
    if (!same)
    {
      LOG_ERROR(path + ": editing it in place doesn't give what was filled; "
                       "it's left as it was");
      ok = false;
      continue;
    }
    if (!write)
      continue;
    if (!sfs::writeFileAtomically(file, *result))
    {
      LOG_ERROR(path + ": can't be written");
      ok = false;
      continue;
    }
    store.forgetPrefab(record->id); // a prefab holding it reads it again
  }
  clear();
  report.ok = ok;
  return report;
}
} // namespace

FillPartsReport
fillProjectParts(const fs::path& project, std::string_view which, bool write)
{
  // Nothing in a malformed file may end the tool: it reports and stops.
  try
  {
    return fill(project, which, write);
  }
  catch (const std::exception& error)
  {
    LOG_ERROR(std::string("Filling parts stopped (") + error.what() + ")");
    return {};
  }
}
