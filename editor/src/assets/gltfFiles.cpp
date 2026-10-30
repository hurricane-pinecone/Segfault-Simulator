#include "assets/gltfFiles.h"

#include "engine/core/logger/logger.h"
#include "engine/core/util/atomicFile.h"

#include "nlohmann/json.hpp"

#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace gltfFiles
{

namespace
{
bool isGltf(const fs::path& path) { return path.extension() == ".gltf"; }

bool isDataUri(const std::string& uri) { return uri.starts_with("data:"); }

// The .gltf's references, made to work from its new place: `newDataName` (the
// .bin, which moved with it) by its new name, anything else re-pointed from
// the new folder to where it still is. @return whether anything changed
bool repoint(nlohmann::json& gltf,
             const fs::path& oldFolder,
             const fs::path& newFolder,
             const std::string& oldDataName,
             const std::string& newDataName)
{
  bool changed = false;
  for (const char* list : {"buffers", "images"})
  {
    if (!gltf.contains(list) || !gltf[list].is_array())
      continue;
    for (nlohmann::json& item : gltf[list])
    {
      if (!item.is_object() || !item.contains("uri") || !item["uri"].is_string())
        continue;
      const std::string uri = item["uri"].get<std::string>();
      if (isDataUri(uri))
        continue;
      std::string updated = uri;
      if (!oldDataName.empty() && uri == oldDataName)
        updated = newDataName;
      else if (oldFolder != newFolder)
        updated = (oldFolder / uri).lexically_normal().lexically_relative(newFolder).generic_string();
      if (updated != uri)
      {
        item["uri"] = updated;
        changed = true;
      }
    }
  }
  return changed;
}
} // namespace

const sfs::AssetRecord* companionOf(const sfs::AssetDatabase& assets, const sfs::AssetRecord& gltf)
{
  if (!isGltf(gltf.path))
    return nullptr;
  fs::path data = gltf.path;
  data.replace_extension(".bin");
  return assets.findByPath(data);
}

bool isCompanion(const sfs::AssetDatabase& assets, const sfs::AssetRecord& record)
{
  if (record.path.extension() != ".bin")
    return false;
  fs::path model = record.path;
  model.replace_extension(".gltf");
  return assets.findByPath(model) != nullptr;
}

bool move(sfs::AssetDatabase& assets, const sfs::AssetId& id, const fs::path& relative)
{
  const sfs::AssetRecord* record = assets.find(id);
  if (!record)
    return false;
  if (!isGltf(record->path))
    return assets.move(id, relative);

  // Everything worked out before anything moves (records change as they do).
  const fs::path oldFile = assets.absolutePath(*record);
  const fs::path oldFolder = oldFile.parent_path();
  const fs::path newFolder = (assets.root() / relative).parent_path();
  const sfs::AssetRecord* data = companionOf(assets, *record);
  const std::optional<sfs::AssetId> dataId = data ? std::optional<sfs::AssetId>(data->id) : std::nullopt;
  fs::path newData = relative;
  newData.replace_extension(".bin");
  const std::string oldDataName = data ? data->path.filename().string() : std::string();

  std::ifstream in(oldFile, std::ios::binary);
  nlohmann::json gltf = nlohmann::json::parse(in, nullptr, false);
  in.close();

  if (!assets.move(id, relative))
    return false;
  if (dataId && !assets.move(*dataId, newData))
    LOG_ERROR("Moved " + relative.generic_string() + " but not its data (" + oldDataName + ")");

  if (!gltf.is_discarded() &&
      repoint(gltf, oldFolder, newFolder, dataId ? oldDataName : std::string(), newData.filename().string()))
  {
    if (!sfs::writeFileAtomically(assets.root() / relative, gltf.dump(2) + '\n'))
      LOG_ERROR("Moved " + relative.generic_string() + " but couldn't update the paths in it");
  }
  return true;
}

bool remove(sfs::AssetDatabase& assets, const sfs::AssetId& id)
{
  const sfs::AssetRecord* record = assets.find(id);
  if (!record)
    return false;
  const sfs::AssetRecord* data = companionOf(assets, *record);
  const std::optional<sfs::AssetId> dataId = data ? std::optional<sfs::AssetId>(data->id) : std::nullopt;
  if (!assets.remove(id))
    return false;
  if (dataId)
    assets.remove(*dataId);
  return true;
}

} // namespace gltfFiles
