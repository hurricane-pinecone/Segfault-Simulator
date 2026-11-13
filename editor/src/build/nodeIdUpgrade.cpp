#include "build/nodeIdUpgrade.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/core/util/atomicFile.h"
#include "engine/core/util/jsonTokens.h"
#include "engine/core/util/readFileText.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/modelImporter.h"
#include "engine/runtime/serialization/sceneSerializer.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <iterator>
#include <map>
#include <optional>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::json;
using NodeIdEdit = sfs::SceneSerializer::NodeIdEdit;

namespace
{
// One prefab or scene file, as read and as upgraded.
struct File
{
  fs::path path;
  std::string relative;
  bool scene = false;
  std::string text;
  json before;
  std::vector<NodeIdEdit> edits;
  std::string upgradedText;
  json after;
};

// A model's nodes and their name paths, by index.
struct ModelNodes
{
  std::vector<sfs::ModelNode> nodes;
  std::vector<std::string> paths;
};

// The file's own form of a string: what writePretty() writes for it.
std::string quoted(const std::string& text) { return json(text).dump(); }

// The file with the edits made in memory, and its format raised to `version`.
std::optional<json> upgradedInMemory(const json& file, const std::vector<NodeIdEdit>& edits, int version)
{
  json out = file;
  try
  {
    for (const NodeIdEdit& edit : edits)
    {
      const json::json_pointer pointer(edit.pointer);
      if (!edit.key)
      {
        out.at(pointer) = edit.to;
        continue;
      }
      json& object = out.at(pointer.parent_pointer());
      json value = std::move(object.at(edit.from));
      object.erase(edit.from);
      object[edit.to] = std::move(value);
    }
  }
  catch (const json::exception& error)
  {
    LOG_ERROR(std::string("Can't make an edit in memory: ") + error.what());
    return std::nullopt;
  }
  out["formatVersion"] = version;
  return out;
}

// The file's text with the edits made to exactly their strings (and its "formatVersion" number); nothing (logged)
// if a string isn't where the edit says.
std::optional<std::string> upgradedText(const File& file, int version)
{
  const std::optional<std::vector<sfs::JsonToken>> tokens = sfs::scanJsonTokens(file.text);
  if (!tokens)
  {
    LOG_ERROR(file.relative + ": not valid JSON");
    return std::nullopt;
  }
  std::unordered_map<std::string, const sfs::JsonToken*> keys, strings;
  const sfs::JsonToken* format = nullptr;
  for (const sfs::JsonToken& token : *tokens)
    if (token.kind == sfs::JsonToken::Kind::Key)
      keys[token.pointer] = &token;
    else if (token.kind == sfs::JsonToken::Kind::String)
      strings[token.pointer] = &token;
    else if (token.kind == sfs::JsonToken::Kind::Number && token.pointer == "/formatVersion")
      format = &token;

  std::vector<sfs::TextEdit> edits;
  edits.reserve(file.edits.size() + 1);
  for (const NodeIdEdit& edit : file.edits)
  {
    const auto& tokensOfKind = edit.key ? keys : strings;
    const auto token = tokensOfKind.find(edit.pointer);
    if (token == tokensOfKind.end() || token->second->text != edit.from)
    {
      LOG_ERROR(file.relative + ": no '" + edit.from + "' at " + edit.pointer);
      return std::nullopt;
    }
    edits.push_back({token->second->begin, token->second->end, quoted(edit.to)});
  }
  if (!format)
  {
    LOG_ERROR(file.relative + ": has no formatVersion to raise");
    return std::nullopt;
  }
  if (format->text != std::to_string(version))
    edits.push_back({format->begin, format->end, std::to_string(version)});
  std::optional<std::string> text = sfs::applyTextEdits(file.text, std::move(edits));
  if (!text)
    LOG_ERROR(file.relative + ": the edits overlap");
  return text;
}

// The entities every entry of a file makes (a prefab's root, each of a scene's entries), in order.
std::vector<sfs::SceneSerializer::ExpandedEntity> expandFile(const sfs::SceneSerializer& serializer, const json& file)
{
  std::vector<sfs::SceneSerializer::ExpandedEntity> out;
  const auto append = [&](const json& entry)
  {
    std::vector<sfs::SceneSerializer::ExpandedEntity> entities = serializer.expandEntry(entry);
    std::move(entities.begin(), entities.end(), std::back_inserter(out));
  };
  if (const auto root = file.find("root"); root != file.end())
    append(*root);
  if (const auto entities = file.find("entities"); entities != file.end() && entities->is_array())
    for (const json& entry : *entities)
      append(entry);
  return out;
}

bool sameEntities(const std::vector<sfs::SceneSerializer::ExpandedEntity>& a,
                  const std::vector<sfs::SceneSerializer::ExpandedEntity>& b)
{
  return std::equal(a.begin(),
                    a.end(),
                    b.begin(),
                    b.end(),
                    [](const auto& x, const auto& y)
                    { return x.key == y.key && x.parent == y.parent && x.components == y.components; });
}
} // namespace

namespace
{
// Writes every upgraded file, or none: each is staged beside its file first, so a full disk or a read-only file stops
// it before any file is replaced, and a replace that fails puts the ones replaced before it back.
bool writeAll(const std::vector<File>& files)
{
  std::vector<const File*> changed;
  for (const File& file : files)
    if (!file.edits.empty())
      changed.push_back(&file);
  const auto stageOf = [](const File& file) { return fs::path(file.path.string() + ".upgrading"); };
  const auto unstage = [&]
  {
    std::error_code ignored;
    for (const File* file : changed)
      fs::remove(stageOf(*file), ignored);
  };
  for (const File* file : changed)
    if (!sfs::writeFileAtomically(stageOf(*file), file->upgradedText) ||
        sfs::readFileText(stageOf(*file)) != file->upgradedText)
    {
      LOG_ERROR("Couldn't stage " + file->relative + "; no file was changed");
      unstage();
      return false;
    }
  // Each file must still be as it was read, so a change made meanwhile is never overwritten.
  for (const File* file : changed)
    if (sfs::readFileText(file->path) != file->text)
    {
      LOG_ERROR(file->relative + " changed while it was being upgraded; no file was changed");
      unstage();
      return false;
    }
  for (std::size_t i = 0; i < changed.size(); ++i)
    if (!sfs::writeFileAtomically(changed[i]->path, changed[i]->upgradedText) ||
        sfs::readFileText(changed[i]->path) != changed[i]->upgradedText)
    {
      LOG_ERROR("Couldn't write " + changed[i]->relative + "; putting back the files already upgraded");
      for (std::size_t j = 0; j <= i; ++j)
        if (!sfs::writeFileAtomically(changed[j]->path, changed[j]->text))
          LOG_ERROR("Couldn't put back " + changed[j]->relative + "; restore it from version control");
      unstage();
      return false;
    }
  unstage();
  return true;
}

NodeIdUpgradeReport upgrade(const fs::path& project, bool write)
{
  NodeIdUpgradeReport report;
  std::error_code error;
  if (!fs::is_directory(project, error))
  {
    LOG_ERROR("No project folder at " + project.string());
    return report;
  }
  const sfs::AssetDatabase database(project, sfs::AssetDatabase::Mode::ReadOnly, sfs::AssetDatabase::projectFolders());
  sfs::AssetStore store;
  store.setDatabase(&database);

  // Every prefab (by its asset name) and every scene, in path order.
  std::vector<File> files;
  std::unordered_map<std::string, std::size_t> prefabByName;
  for (const sfs::AssetRecord& record : database.records())
    if (record.kind == sfs::AssetKind::Prefab)
    {
      prefabByName["asset:" + record.id.toString()] = files.size();
      files.push_back({database.absolutePath(record), record.path.generic_string()});
    }
  std::vector<fs::path> scenes;
  for (auto it = fs::recursive_directory_iterator(project / "scenes", error); !error && it != fs::end(it);
       it.increment(error))
    if (std::error_code kind; it->is_regular_file(kind) && it->path().extension() == ".scene")
      scenes.push_back(it->path());
  std::sort(scenes.begin(), scenes.end());
  for (const fs::path& scene : scenes)
    files.push_back({scene, scene.lexically_relative(project).generic_string(), true});

  for (File& file : files)
  {
    std::optional<std::string> text = sfs::readFileText(file.path);
    file.before = text ? json::parse(*text, nullptr, false) : json();
    if (!text || file.before.is_discarded() || !file.before.is_object())
    {
      LOG_ERROR(file.relative + ": can't be read as JSON; nothing is upgraded");
      return report;
    }
    file.text = std::move(*text);
  }
  report.files = files.size();

  // Prefabs come from the files as they were, or as upgraded; models from the asset store, as instances see them.
  bool upgraded = false;
  sfs::SceneSerializer serializer(sfs::reflect::types(), nullptr);
  serializer.setPrefabSource(
      [&](std::string_view name) -> std::optional<json>
      {
        if (const auto it = prefabByName.find(std::string(name)); it != prefabByName.end())
          return upgraded ? files[it->second].after : files[it->second].before;
        const std::optional<sfs::AssetMeshName> asset = sfs::parseAssetMeshName(name);
        return asset && asset->part.empty() ? store.prefab(asset->model) : std::nullopt;
      });

  bool ok = true;
  for (File& file : files)
  {
    std::vector<std::string> unreadable;
    file.edits = serializer.nodeIdEdits(file.before, &unreadable);
    for (const std::string& prefab : unreadable)
    {
      LOG_ERROR(file.relative + ": can't read the prefab " + prefab + ", so its instance's paths can't be upgraded");
      report.unreadable.push_back(file.relative + ": " + prefab);
      ok = false;
    }
    file.after = file.before;
    if (file.edits.empty())
      continue;
    const int version = file.scene ? sfs::SceneSerializer::kSceneFormatVersion
                                   : sfs::SceneSerializer::kPrefabFormatVersion;
    std::optional<std::string> text = upgradedText(file, version);
    if (!text)
    {
      ok = false;
      continue;
    }
    file.upgradedText = std::move(*text);
    file.after = json::parse(file.upgradedText, nullptr, false);
    // Check: the edited text parses to the in-memory upgrade, so nothing else changed.
    const std::optional<json> expected = upgradedInMemory(file.before, file.edits, version);
    if (file.after.is_discarded() || !expected || file.after != *expected)
    {
      LOG_ERROR(file.relative + ": the edited file isn't the upgrade of the old one");
      ok = false;
      file.after = file.before;
    }
    NodeIdUpgradeReport::ChangedFile changed{file.relative, file.edits.size()};
    for (const NodeIdEdit& edit : file.edits)
    {
      ++(edit.key                              ? report.overrideKeys
         : edit.pointer.ends_with("/$parent") ? report.addedParents
                                               : report.removedPaths);
      if (edit.toId == 0)
        ++report.oldIdsKept;
    }
    report.changed.push_back(std::move(changed));
  }

  // Check: each edit names a node with the same name path by its old id (its place in the model file's order) as by
  // its stable id, read straight from the model file. One past the model's nodes must be past them there too.
  std::map<std::string, std::optional<ModelNodes>> models;
  for (const File& file : files)
    for (const NodeIdEdit& edit : file.edits)
    {
      auto [model, fresh] = models.try_emplace(edit.model);
      if (fresh)
        if (const std::optional<sfs::AssetMeshName> asset = sfs::parseAssetMeshName(edit.model))
          if (const sfs::AssetRecord* record = database.find(asset->model))
          {
            ModelNodes read;
            if (std::optional<std::vector<sfs::ModelNode>> nodes =
                    sfs::importModelNodes(database.absolutePath(*record),
                                          sfs::ModelImportSettings::fromJson(record->settings),
                                          &read.paths))
            {
              read.nodes = std::move(*nodes);
              model->second = std::move(read);
            }
          }
      const std::optional<ModelNodes>& nodes = model->second;
      if (edit.toId == 0)
      {
        if (!nodes || (edit.fromId >= 1 && edit.fromId <= nodes->nodes.size()))
        {
          LOG_ERROR(file.relative + ": '" + edit.from + "' names a node of " + edit.model + " but wasn't upgraded");
          ok = false;
        }
        else
          LOG_WARN(file.relative + ": '" + edit.from + "' is past the nodes of " + edit.model + "; kept unused as '" +
                   edit.to + "'");
        continue;
      }
      const auto after = nodes ? std::find_if(nodes->nodes.begin(),
                                              nodes->nodes.end(),
                                              [&](const sfs::ModelNode& node) { return node.id == edit.toId; })
                               : std::vector<sfs::ModelNode>::const_iterator();
      if (!nodes || edit.fromId < 1 || edit.fromId > nodes->nodes.size() || after == nodes->nodes.end() ||
          nodes->paths[edit.fromId - 1] != nodes->paths[after - nodes->nodes.begin()])
      {
        LOG_ERROR(file.relative + ": '" + edit.from + "' -> '" + edit.to + "' doesn't name the same node of " +
                  edit.model);
        ok = false;
      }
    }

  // Check: every entry makes the same entities from the old files (their old ids read as the nodes they name) as
  // from the upgraded ones; and the upgraded files need no more edits.
  std::vector<std::vector<sfs::SceneSerializer::ExpandedEntity>> expandedBefore;
  expandedBefore.reserve(files.size());
  for (const File& file : files)
    expandedBefore.push_back(expandFile(serializer, file.before));
  upgraded = true;
  for (std::size_t i = 0; i < files.size(); ++i)
  {
    if (!sameEntities(expandedBefore[i], expandFile(serializer, files[i].after)))
    {
      LOG_ERROR(files[i].relative + ": makes different entities once upgraded");
      ok = false;
    }
    if (!serializer.nodeIdEdits(files[i].after).empty())
    {
      LOG_ERROR(files[i].relative + ": would still change if upgraded again");
      ok = false;
    }
  }

  if (!ok)
  {
    LOG_ERROR("Upgrading model node ids failed its checks; no file was changed");
    return report;
  }
  report.ok = true;
  if (write)
    report.ok = writeAll(files);
  return report;
}
} // namespace

NodeIdUpgradeReport upgradeNodeIds(const fs::path& project, bool write)
{
  // Nothing in a malformed file may end the tool: it reports and changes nothing.
  try
  {
    return upgrade(project, write);
  }
  catch (const std::exception& error)
  {
    LOG_ERROR(std::string("Upgrading model node ids stopped (") + error.what() + "); no file was changed");
    return {};
  }
}

int upgradeNodeIdsHeadless(const fs::path& project, bool checkOnly)
{
  const NodeIdUpgradeReport report = upgradeNodeIds(project, !checkOnly);
  for (const NodeIdUpgradeReport::ChangedFile& file : report.changed)
    LOG_INFO(file.path + ": " + std::to_string(file.edits) + " model node ids to upgrade");
  LOG_INFO(std::to_string(report.files) + " prefab and scene files read; " + std::to_string(report.changed.size()) +
           " with old model node ids: " + std::to_string(report.overrideKeys) + " override keys, " +
           std::to_string(report.removedPaths) + " removed paths, " + std::to_string(report.addedParents) +
           " added entities' parents (" + std::to_string(report.oldIdsKept) + " past their model's nodes); " +
           std::to_string(report.unreadable.size()) + " instances whose prefab can't be read");
  if (!report.ok)
    return 1;
  LOG_INFO(checkOnly ? std::string("Every check passed; nothing was written (--check)")
                     : std::to_string(report.changed.size()) + " files upgraded");
  return 0;
}

int listNodeIdsHeadless(const fs::path& model)
{
  std::vector<std::string> paths;
  const std::optional<std::vector<sfs::ModelNode>> nodes = sfs::importModelNodes(model, {}, &paths);
  if (!nodes)
    return 1;
  std::printf("%-6s %-10s %s\n", "old", "stable", "name path");
  for (std::size_t i = 0; i < nodes->size(); ++i)
    std::printf("%-6zu %-10u %s\n", i + 1, (*nodes)[i].id, paths[i].c_str());
  return 0;
}
