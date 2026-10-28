#include "engine/runtime/scriptableObjects/scriptableObjects.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/runtime/serialization/sceneSerializer.h"

#include <algorithm>
#include <fstream>

namespace sfs
{

namespace
{
constexpr std::string_view kPrefix = "asset:";

// The catalogue record an "asset:<id>" names.
const AssetRecord* recordOf(const AssetDatabase* catalogue, std::string_view asset)
{
  if (!catalogue || !asset.starts_with(kPrefix))
    return nullptr;
  const std::optional<AssetId> id = AssetId::parse(asset.substr(kPrefix.size()));
  return id ? catalogue->find(*id) : nullptr;
}

std::optional<nlohmann::json> readFile(const AssetDatabase& catalogue, const AssetRecord& record)
{
  std::ifstream in(catalogue.absolutePath(record));
  if (!in)
    return std::nullopt;
  nlohmann::json contents = nlohmann::json::parse(in, nullptr, false);
  if (contents.is_discarded() || !contents.is_object())
    return std::nullopt;
  return contents;
}
} // namespace

// At exit, objects left may be of a game module's types, and it may be gone:
// left as they are (clear() before unloading one).
ScriptableObjects::~ScriptableObjects() = default;

void ScriptableObjects::setCatalogue(const AssetDatabase* catalogue)
{
  clear();
  m_catalogue = catalogue;
}

void ScriptableObjects::setMeshes(MeshLibrary* meshes)
{
  if (meshes == m_meshes)
    return;
  clear();
  m_meshes = meshes;
}

const ScriptableObject* ScriptableObjects::get(std::string_view asset)
{
  if (const auto it = m_loaded.find(asset); it != m_loaded.end())
    return it->second.object;

  // Read once, even if it fails (logged once).
  Loaded& loaded = m_loaded[std::string(asset)];
  const AssetRecord* record = recordOf(m_catalogue, asset);
  if (!record)
  {
    if (m_catalogue)
      LOG_ERROR("No scriptable object " + std::string(asset));
    return nullptr;
  }
  const std::optional<nlohmann::json> contents = readFile(*m_catalogue, *record);
  const std::string type = contents ? contents->value("type", std::string()) : std::string();
  const reflect::TypeInfo* info = reflect::types().find(type);
  if (!info || info->kind != reflect::TypeKind::ScriptableObject || !info->scriptable.create)
  {
    LOG_ERROR("Can't load " + record->path.generic_string() +
              (type.empty() ? ": it isn't a scriptable object" : ": no scriptable object type '" + type + "' (are the scripts built?)"));
    return nullptr;
  }

  loaded.type = type;
  loaded.object = static_cast<ScriptableObject*>(info->scriptable.create());
  loaded.destroy = info->scriptable.destroy;
  if (const auto fields = contents->find("fields"); fields != contents->end() && fields->is_object())
    SceneSerializer(reflect::types(), m_meshes).loadObject(*info, loaded.object, *fields);
  return loaded.object;
}

const std::string& ScriptableObjects::typeOf(const std::string& asset)
{
  if (const auto it = m_types.find(asset); it != m_types.end())
    return it->second;
  std::string type;
  if (const AssetRecord* record = recordOf(m_catalogue, asset))
    if (const std::optional<nlohmann::json> contents = readFile(*m_catalogue, *record))
      type = contents->value("type", std::string());
  return m_types[asset] = type;
}

std::vector<ScriptableObjects::Entry> ScriptableObjects::list(std::string_view type)
{
  std::vector<Entry> entries;
  if (!m_catalogue)
    return entries;
  for (const AssetRecord& record : m_catalogue->records())
  {
    if (record.kind != AssetKind::ScriptableObject)
      continue;
    const std::string asset = std::string(kPrefix) + record.id.toString();
    const std::string& recordType = typeOf(asset);
    if (type.empty() || recordType == type)
      entries.push_back({asset, record.path.stem().string(), recordType});
  }
  std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) { return a.name < b.name; });
  return entries;
}

void ScriptableObjects::forget(std::string_view asset)
{
  if (const auto it = m_loaded.find(asset); it != m_loaded.end())
  {
    if (it->second.object && it->second.destroy)
      it->second.destroy(it->second.object);
    m_loaded.erase(it);
  }
  if (const auto it = m_types.find(asset); it != m_types.end())
    m_types.erase(it);
}

void ScriptableObjects::clear()
{
  for (auto& [asset, loaded] : m_loaded)
    if (loaded.object && loaded.destroy)
      loaded.destroy(loaded.object);
  m_loaded.clear();
  m_types.clear();
}

nlohmann::json ScriptableObjects::fileContents(const std::string& type, const nlohmann::json& fields)
{
  return {{"formatVersion", 1}, {"type", type}, {"fields", fields}};
}

ScriptableObjects& scriptableObjects()
{
  static ScriptableObjects assets;
  return assets;
}

} // namespace sfs
