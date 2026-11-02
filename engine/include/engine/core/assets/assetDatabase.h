#pragma once

#include "nlohmann/json.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sfs
{

/**
 * An asset's stable identity: 128 random bits, written as 32 hex digits. It
 * lives in the asset's .meta file, so it survives renames and moves, and is
 * what scenes and code should keep to refer to an asset.
 */
struct AssetId
{
  uint64_t high = 0;
  uint64_t low = 0;

  /** A new, random id. */
  static AssetId generate();

  /** Read 32 hex digits; nothing if `text` isn't one. */
  static std::optional<AssetId> parse(std::string_view text);

  std::string toString() const;
  bool valid() const { return high != 0 || low != 0; }

  auto operator<=>(const AssetId&) const = default;
};

struct AssetIdHash
{
  std::size_t operator()(const AssetId& id) const
  {
    return std::hash<uint64_t>()(id.high ^ (id.low * 31));
  }
};

/** What an asset is, from its file extension. */
enum class AssetKind
{
  Texture,          // .png .jpg .jpeg .bmp .tga
  Model,            // .glb .gltf .obj
  Material,         // .mat (see MaterialAsset)
  Prefab,           // .prefab (see SceneSerializer)
  VisualStyle,      // .style: a scene's whole look (see VisualStyle)
  ScriptableObject, // .asset: a scriptable object type's values (see
                    // ScriptableObject, ScriptableObjects)
  Font,             // .ttf .otf
  Audio,            // .wav .ogg .mp3 .flac
  Data,             // .json (e.g. an Aseprite atlas), .txt, .csv
  Other,
};

AssetKind assetKindOf(const std::filesystem::path& file);
const char* assetKindName(AssetKind kind);

/** One file in an AssetDatabase. */
struct AssetRecord
{
  AssetId id;
  std::filesystem::path
      path; // relative to the database's root, e.g. "assets/models/barn.glb"
  AssetKind kind = AssetKind::Other;
  nlohmann::json settings =
      nlohmann::json::object(); // import settings, from the .meta
};

/**
 * The catalogue of a project's asset files: every file in some folders of a
 * root (a project's assets/ and prefabs/), or under the whole root, each with
 * a stable AssetId kept in a sidecar "<file>.meta" next to it. Shared by every
 * kind of game (2D, isometric, 3D) and by the editor; it knows files, not their
 * contents. Loading what's in them is the AssetStore's job (and the renderers',
 * for GPU copies).
 *
 * The editor opens it read-write: refresh() gives new files a .meta, and the
 * editing calls move, rename and delete a file together with its .meta, so
 * its id follows it. A shipped game opens it read-only and never writes.
 *
 * Commit .meta files with their assets: a file moved without its .meta (e.g.
 * in Explorer) gets a new id, and whatever referred to the old one loses it.
 */
class AssetDatabase
{
public:
  static constexpr const char* kMetaExtension = ".meta";

  /** A game project's catalogued folders: assets/ (models, textures...) and
   * prefabs/. */
  static std::vector<std::filesystem::path> projectFolders()
  {
    return {"assets", "prefabs"};
  }

  enum class Mode
  {
    ReadWrite, // the editor
    ReadOnly,  // a shipped game
  };

  /**
   * @param folders the root's folders to catalogue (e.g. {"assets",
   *                "prefabs"} in a project); empty: everything under the root
   */
  explicit AssetDatabase(std::filesystem::path root,
                         Mode mode = Mode::ReadWrite,
                         std::vector<std::filesystem::path> folders = {});

  const std::filesystem::path& root() const { return m_root; }
  Mode mode() const { return m_mode; }

  /**
   * Rescan the root. Read-write, it also writes a .meta for each file that has
   * none, and gives a fresh id to a copy: of two files claiming one id, the
   * one that had it in the catalogue before keeps it (else the first by path),
   * so "a - Copy.png" never takes "a.png"'s id however the names sort.
   *
   * A .meta that can't be read (hand-edited, a merge conflict left in it) is
   * never replaced: that would lose the id everything refers to the asset by.
   * Its file is listed without an id, an error is logged, and it gets its id
   * back once the .meta is fixed. (An empty .meta, from a crash in an older
   * version, has no id to keep, and gets a new one.)
   *
   * @return true if the catalogue changed
   */
  bool refresh();

  /**
   * A refresh in two halves, so the slow one can run on another thread:
   * scan() reads the folders (and, read-write, writes .meta files for new
   * ones) touching nothing of a database; apply() takes the result in.
   */
  struct Scan
  {
    std::vector<AssetRecord> records;
    std::vector<std::filesystem::path> folders;
  };
  /** @param previous the catalogue before (whose ids copies don't take) */
  static Scan scan(const std::filesystem::path& root,
                   Mode mode,
                   const std::vector<std::filesystem::path>& folders,
                   const std::vector<AssetRecord>& previous = {});
  Scan scan() const { return scan(m_root, m_mode, m_scanned, m_records); }
  /** @return true if the catalogue changed */
  bool apply(Scan scan);

  /** The folders catalogued (empty: the whole root). */
  const std::vector<std::filesystem::path>& scannedFolders() const
  {
    return m_scanned;
  }

  /** Every asset, sorted by path. */
  const std::vector<AssetRecord>& records() const { return m_records; }

  const AssetRecord* find(const AssetId& id) const;
  const AssetRecord* findByPath(const std::filesystem::path& relative) const;

  /** Where the asset's file is on disk. */
  std::filesystem::path absolutePath(const AssetRecord& record) const;

  /**
   * Folders under the root (relative, e.g. "assets/models"), sorted; the
   * catalogued folders themselves included, when given.
   */
  const std::vector<std::filesystem::path>& folders() const
  {
    return m_folders;
  }

  /** Bumped whenever the catalogue changes, so a UI can tell it should redraw.
   */
  uint64_t version() const { return m_version; }

  // --- editing (read-write only; each returns false, logged, on failure)
  // ------

  /**
   * Move or rename an asset (file and .meta) to `relative`, creating folders.
   * Refuses to overwrite an existing file.
   */
  bool move(const AssetId& id, const std::filesystem::path& relative);

  /** Move or rename a folder, with everything in it. */
  bool moveFolder(const std::filesystem::path& from,
                  const std::filesystem::path& to);

  /** Delete an asset's file and .meta. */
  bool remove(const AssetId& id);

  /** Delete a folder and every asset in it. */
  bool removeFolder(const std::filesystem::path& relative);

  bool createFolder(const std::filesystem::path& relative);

  /**
   * Copy a file from outside the project into `folder` (relative), as a new
   * asset. A name already taken gets a number added ("tree 2.png").
   *
   * @return the new asset's id
   */
  std::optional<AssetId> import(const std::filesystem::path& file,
                                const std::filesystem::path& folder);

  /** Replace an asset's import settings, and save them to its .meta. */
  bool setSettings(const AssetId& id, nlohmann::json settings);

private:
  struct Meta
  {
    AssetId id;
    nlohmann::json settings;
  };

  bool writable(const char* action) const;
  static std::filesystem::path metaPathFor(const std::filesystem::path& file);
  enum class MetaState
  {
    Read,
    Missing, // no .meta, or an empty one
    Corrupt, // there, but not something with an id in it
  };
  static MetaState readMeta(const std::filesystem::path& file,
                            std::optional<Meta>& meta);
  static bool writeMeta(const std::filesystem::path& file, const Meta& meta);
  void rebuildIndex();
  // After an edit made here: re-sort and re-index, and bump the version.
  void catalogueChanged();
  // Add a folder and the folders above it to the list (if missing).
  void addFolders(const std::filesystem::path& relative);

  std::filesystem::path m_root;
  Mode m_mode;
  std::vector<std::filesystem::path>
      m_scanned; // the folders catalogued; empty: the root
  std::vector<AssetRecord> m_records;
  std::vector<std::filesystem::path> m_folders;
  std::unordered_map<AssetId, std::size_t, AssetIdHash> m_byId;
  std::unordered_map<std::string, std::size_t> m_byPath; // generic path strings
  uint64_t m_version = 0;
};

} // namespace sfs
