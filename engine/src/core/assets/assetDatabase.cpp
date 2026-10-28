#include "engine/core/assets/assetDatabase.h"

#include "engine/core/logger/logger.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <system_error>
#include <unordered_set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace sfs
{

namespace
{
constexpr int kMetaFormatVersion = 1;

std::string lowercase(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

bool hidden(const fs::path& path) { return path.filename().string().starts_with("."); }

bool isMeta(const fs::path& path) { return path.extension() == AssetDatabase::kMetaExtension; }

std::string key(const fs::path& relative) { return relative.lexically_normal().generic_string(); }

bool sameRecord(const AssetRecord& a, const AssetRecord& b)
{
  return a.id == b.id && a.path == b.path && a.kind == b.kind && a.settings == b.settings;
}
} // namespace

// --- AssetId ------------------------------------------------------------------------

AssetId AssetId::generate()
{
  static thread_local std::mt19937_64 random{std::random_device{}() ^
                                             (static_cast<uint64_t>(std::random_device{}()) << 32)};
  AssetId id;
  while (!id.valid())
    id = AssetId{random(), random()};
  return id;
}

std::optional<AssetId> AssetId::parse(std::string_view text)
{
  if (text.size() != 32)
    return std::nullopt;
  AssetId id;
  for (std::size_t i = 0; i < 32; ++i)
  {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
    int digit = 0;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (c >= 'a' && c <= 'f')
      digit = c - 'a' + 10;
    else
      return std::nullopt;
    uint64_t& half = i < 16 ? id.high : id.low;
    half = (half << 4) | static_cast<uint64_t>(digit);
  }
  return id;
}

std::string AssetId::toString() const
{
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text(32, '0');
  for (int i = 0; i < 16; ++i)
  {
    text[15 - i] = kDigits[(high >> (4 * i)) & 0xf];
    text[31 - i] = kDigits[(low >> (4 * i)) & 0xf];
  }
  return text;
}

// --- kinds ----------------------------------------------------------------------------

AssetKind assetKindOf(const fs::path& file)
{
  const std::string extension = lowercase(file.extension().string());
  const auto any = [&](std::initializer_list<const char*> list)
  { return std::any_of(list.begin(), list.end(), [&](const char* e) { return extension == e; }); };

  if (any({".png", ".jpg", ".jpeg", ".bmp", ".tga"}))
    return AssetKind::Texture;
  if (any({".glb", ".gltf", ".obj"}))
    return AssetKind::Model;
  if (any({".mat"}))
    return AssetKind::Material;
  if (any({".prefab"}))
    return AssetKind::Prefab;
  if (any({".ttf", ".otf"}))
    return AssetKind::Font;
  if (any({".wav", ".ogg", ".mp3", ".flac"}))
    return AssetKind::Audio;
  if (any({".json", ".txt", ".csv"}))
    return AssetKind::Data;
  return AssetKind::Other;
}

const char* assetKindName(AssetKind kind)
{
  switch (kind)
  {
  case AssetKind::Texture:
    return "Texture";
  case AssetKind::Model:
    return "Model";
  case AssetKind::Material:
    return "Material";
  case AssetKind::Prefab:
    return "Prefab";
  case AssetKind::Font:
    return "Font";
  case AssetKind::Audio:
    return "Audio";
  case AssetKind::Data:
    return "Data";
  case AssetKind::Other:
    break;
  }
  return "File";
}

// --- AssetDatabase ------------------------------------------------------------------------

AssetDatabase::AssetDatabase(fs::path root, Mode mode, std::vector<fs::path> folders)
    : m_root(std::move(root)), m_mode(mode), m_scanned(std::move(folders))
{
  refresh();
}

bool AssetDatabase::writable(const char* action) const
{
  if (m_mode == Mode::ReadWrite)
    return true;
  LOG_ERROR(std::string("AssetDatabase: can't ") + action + " in a read-only database");
  return false;
}

fs::path AssetDatabase::metaPathFor(const fs::path& file)
{
  fs::path meta = file;
  meta += kMetaExtension;
  return meta;
}

std::optional<AssetDatabase::Meta> AssetDatabase::readMeta(const fs::path& file)
{
  std::ifstream in(metaPathFor(file), std::ios::binary);
  if (!in)
    return std::nullopt;
  const json data = json::parse(in, nullptr, false);
  if (!data.is_object() || !data.contains("id") || !data["id"].is_string())
    return std::nullopt;
  const std::optional<AssetId> id = AssetId::parse(data["id"].get<std::string>());
  if (!id || !id->valid())
    return std::nullopt;
  Meta meta{*id, data.value("settings", json::object())};
  if (!meta.settings.is_object())
    meta.settings = json::object();
  return meta;
}

bool AssetDatabase::writeMeta(const fs::path& file, const Meta& meta)
{
  const json data = {{"formatVersion", kMetaFormatVersion},
                     {"id", meta.id.toString()},
                     {"settings", meta.settings}};
  std::ofstream out(metaPathFor(file), std::ios::binary);
  out << data.dump(2) << '\n';
  if (!out)
  {
    LOG_ERROR("AssetDatabase: can't write " + metaPathFor(file).string());
    return false;
  }
  return true;
}

AssetDatabase::Scan AssetDatabase::scan(const fs::path& root, Mode mode, const std::vector<fs::path>& catalogued)
{
  std::vector<fs::path> files;
  std::vector<fs::path> folders;
  std::error_code error;
  std::vector<fs::path> tops; // where to look
  if (catalogued.empty())
    tops.push_back(root);
  for (const fs::path& folder : catalogued)
    if (fs::is_directory(root / folder, error))
    {
      tops.push_back(root / folder);
      folders.push_back(folder);
    }
  for (const fs::path& top : tops)
  {
    error.clear();
    for (auto it = fs::recursive_directory_iterator(top, error);
         !error && it != fs::recursive_directory_iterator();
         it.increment(error))
    {
      const fs::path& path = it->path();
      if (hidden(path))
      {
        if (it->is_directory(error))
          it.disable_recursion_pending();
        continue;
      }
      const fs::path relative = path.lexically_relative(root);
      if (it->is_directory(error))
        folders.push_back(relative);
      else if (!isMeta(path))
        files.push_back(relative);
    }
  }
  std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return key(a) < key(b); });
  std::sort(folders.begin(), folders.end(), [](const fs::path& a, const fs::path& b) { return key(a) < key(b); });

  std::vector<AssetRecord> records;
  records.reserve(files.size());
  std::unordered_set<AssetId, AssetIdHash> seen;
  for (const fs::path& relative : files)
  {
    const fs::path file = root / relative;
    std::optional<Meta> meta = readMeta(file);

    const bool duplicate = meta && seen.contains(meta->id);
    if (mode == Mode::ReadWrite && (!meta || duplicate))
    {
      if (duplicate)
        LOG_INFO("Assets: " + key(relative) + " has another asset's id (a copy?); giving it its own");
      meta = Meta{AssetId::generate(), meta ? meta->settings : json::object()};
      writeMeta(file, *meta);
    }

    AssetRecord record;
    record.path = fs::path(key(relative));
    record.kind = assetKindOf(relative);
    if (meta && !(duplicate && mode == Mode::ReadOnly))
    {
      record.id = meta->id;
      record.settings = std::move(meta->settings);
      seen.insert(record.id);
    }
    records.push_back(std::move(record));
  }

  return {std::move(records), std::move(folders)};
}

bool AssetDatabase::apply(Scan scan)
{
  const bool changed = scan.records.size() != m_records.size() ||
                       !std::equal(scan.records.begin(), scan.records.end(), m_records.begin(), sameRecord) ||
                       scan.folders != m_folders;
  if (changed)
  {
    m_records = std::move(scan.records);
    m_folders = std::move(scan.folders);
    rebuildIndex();
    ++m_version;
  }
  return changed;
}

bool AssetDatabase::refresh() { return apply(scan()); }


void AssetDatabase::rebuildIndex()
{
  m_byId.clear();
  m_byPath.clear();
  for (std::size_t i = 0; i < m_records.size(); ++i)
  {
    if (m_records[i].id.valid())
      m_byId.emplace(m_records[i].id, i);
    m_byPath.emplace(key(m_records[i].path), i);
  }
}

const AssetRecord* AssetDatabase::find(const AssetId& id) const
{
  const auto it = m_byId.find(id);
  return it == m_byId.end() ? nullptr : &m_records[it->second];
}

const AssetRecord* AssetDatabase::findByPath(const fs::path& relative) const
{
  const auto it = m_byPath.find(key(relative));
  return it == m_byPath.end() ? nullptr : &m_records[it->second];
}

fs::path AssetDatabase::absolutePath(const AssetRecord& record) const { return m_root / record.path; }

bool AssetDatabase::move(const AssetId& id, const fs::path& relative)
{
  if (!writable("move assets"))
    return false;
  const AssetRecord* record = find(id);
  if (!record)
    return false;

  const fs::path from = absolutePath(*record);
  const fs::path to = m_root / relative;
  std::error_code error;
  if (fs::exists(to, error))
  {
    LOG_ERROR("Assets: can't move " + key(record->path) + " to " + key(relative) + ": it exists");
    return false;
  }
  fs::create_directories(to.parent_path(), error);
  fs::rename(from, to, error);
  if (error)
  {
    LOG_ERROR("Assets: can't move " + key(record->path) + ": " + error.message());
    return false;
  }
  if (fs::exists(metaPathFor(from)))
    fs::rename(metaPathFor(from), metaPathFor(to), error);
  if (error)
    LOG_ERROR("Assets: moved " + key(relative) + " but not its .meta: " + error.message());
  refresh();
  return true;
}

bool AssetDatabase::moveFolder(const fs::path& from, const fs::path& to)
{
  if (!writable("move folders"))
    return false;
  const std::string fromKey = key(from);
  const std::string toKey = key(to);
  if (fromKey.empty() || toKey == fromKey || toKey.starts_with(fromKey + "/"))
  {
    LOG_ERROR("Assets: can't move folder " + fromKey + " into itself");
    return false;
  }
  std::error_code error;
  if (fs::exists(m_root / to, error))
  {
    LOG_ERROR("Assets: can't move folder " + fromKey + " to " + toKey + ": it exists");
    return false;
  }
  fs::create_directories((m_root / to).parent_path(), error);
  fs::rename(m_root / from, m_root / to, error); // .meta files go with their assets
  if (error)
  {
    LOG_ERROR("Assets: can't move folder " + fromKey + ": " + error.message());
    return false;
  }
  refresh();
  return true;
}

bool AssetDatabase::remove(const AssetId& id)
{
  if (!writable("delete assets"))
    return false;
  const AssetRecord* record = find(id);
  if (!record)
    return false;
  const fs::path file = absolutePath(*record);
  std::error_code error;
  fs::remove(file, error);
  if (error)
  {
    LOG_ERROR("Assets: can't delete " + key(record->path) + ": " + error.message());
    return false;
  }
  fs::remove(metaPathFor(file), error);
  refresh();
  return true;
}

bool AssetDatabase::removeFolder(const fs::path& relative)
{
  if (!writable("delete folders"))
    return false;
  if (key(relative).empty() || key(relative) == ".")
    return false; // never the root
  std::error_code error;
  fs::remove_all(m_root / relative, error);
  if (error)
  {
    LOG_ERROR("Assets: can't delete folder " + key(relative) + ": " + error.message());
    return false;
  }
  refresh();
  return true;
}

bool AssetDatabase::createFolder(const fs::path& relative)
{
  if (!writable("create folders"))
    return false;
  std::error_code error;
  fs::create_directories(m_root / relative, error);
  if (error)
  {
    LOG_ERROR("Assets: can't create folder " + key(relative) + ": " + error.message());
    return false;
  }
  refresh();
  return true;
}

std::optional<AssetId> AssetDatabase::import(const fs::path& file, const fs::path& folder)
{
  if (!writable("import assets"))
    return std::nullopt;

  // A free name: "tree.png", else "tree 2.png", "tree 3.png", ...
  const fs::path targetFolder = m_root / folder;
  fs::path target = targetFolder / file.filename();
  std::error_code error;
  for (int n = 2; fs::exists(target, error); ++n)
    target = targetFolder / (file.stem().string() + " " + std::to_string(n) + file.extension().string());

  fs::create_directories(targetFolder, error);
  fs::copy_file(file, target, error);
  if (error)
  {
    LOG_ERROR("Assets: can't import " + file.string() + ": " + error.message());
    return std::nullopt;
  }
  refresh();
  const AssetRecord* record = findByPath(target.lexically_relative(m_root));
  return record ? std::optional<AssetId>(record->id) : std::nullopt;
}

bool AssetDatabase::setSettings(const AssetId& id, json settings)
{
  if (!writable("change import settings"))
    return false;
  const auto it = m_byId.find(id);
  if (it == m_byId.end())
    return false;
  AssetRecord& record = m_records[it->second];
  if (!settings.is_object())
    settings = json::object();
  if (!writeMeta(absolutePath(record), Meta{id, settings}))
    return false;
  record.settings = std::move(settings);
  ++m_version;
  return true;
}

} // namespace sfs
