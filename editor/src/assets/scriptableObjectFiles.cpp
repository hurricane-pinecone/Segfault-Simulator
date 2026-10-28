#include "assets/scriptableObjectFiles.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/scriptableObjects/scriptableObjects.h"
#include "engine/runtime/serialization/sceneSerializer.h"

#include <cctype>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace scriptableObjectFiles
{

namespace
{
// `stem`.asset in `folder` (relative), with " 2", " 3"... added until no file
// has that name.
fs::path freeName(const sfs::AssetDatabase& assets, const fs::path& folder, const std::string& stem)
{
  const std::string extension(sfs::ScriptableObjects::kExtension);
  fs::path relative = folder / (stem + extension);
  std::error_code error;
  for (int n = 2; fs::exists(assets.root() / relative, error); ++n)
    relative = folder / (stem + " " + std::to_string(n) + extension);
  return relative;
}

// A file name made of letters, digits, spaces, '-' and '_'.
std::string safeName(std::string name)
{
  for (char& c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '-' && c != '_')
      c = '_';
  return name.empty() ? "Data" : name;
}

bool save(const fs::path& file, const nlohmann::json& contents)
{
  std::ofstream out(file);
  out << contents.dump(2) << "\n";
  return static_cast<bool>(out);
}
} // namespace

std::optional<sfs::AssetId> create(sfs::AssetDatabase& assets,
                                   const fs::path& folder,
                                   const sfs::reflect::TypeInfo& type,
                                   const std::string& name,
                                   sfs::MeshLibrary* meshes)
{
  if (type.kind != sfs::reflect::TypeKind::ScriptableObject || !type.scriptable.create)
    return std::nullopt;
  // The type's defaults, as a fresh object has them.
  void* object = type.scriptable.create();
  const nlohmann::json fields = sfs::SceneSerializer(sfs::reflect::types(), meshes).saveObject(type, object);
  type.scriptable.destroy(object);

  std::error_code error;
  fs::create_directories(assets.root() / folder, error);
  const fs::path relative = freeName(assets, folder, safeName(name));
  if (!save(assets.root() / relative, sfs::ScriptableObjects::fileContents(type.name, fields)))
  {
    LOG_ERROR("Can't write the scriptable object " + relative.generic_string());
    return std::nullopt;
  }
  assets.refresh();
  if (const sfs::AssetRecord* record = assets.findByPath(relative))
    return record->id;
  return std::nullopt;
}

std::optional<nlohmann::json> read(const sfs::AssetDatabase& assets, const sfs::AssetRecord& record)
{
  std::ifstream in(assets.absolutePath(record));
  nlohmann::json contents = nlohmann::json::parse(in, nullptr, false);
  if (contents.is_discarded() || !contents.is_object() || !contents.contains("type"))
  {
    LOG_ERROR("Can't read the scriptable object " + record.path.generic_string());
    return std::nullopt;
  }
  return contents;
}

bool write(const sfs::AssetDatabase& assets,
           const sfs::AssetRecord& record,
           const std::string& type,
           const nlohmann::json& fields)
{
  if (save(assets.absolutePath(record), sfs::ScriptableObjects::fileContents(type, fields)))
    return true;
  LOG_ERROR("Can't write the scriptable object " + record.path.generic_string());
  return false;
}

} // namespace scriptableObjectFiles
