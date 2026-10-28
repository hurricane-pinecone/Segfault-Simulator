#pragma once

#include "engine/core/assets/scriptableObject.h"

#include "nlohmann/json.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace sfs
{

class AssetDatabase;
class MeshLibrary;

/**
 * A field referring to a scriptable object of type T (an
 * sfs::ScriptableObject). The editor picks it from the project's assets of that
 * type; get() gives the asset, loaded the first time anything asks for it and
 * shared by everything that does.
 *
 *   sfs::AssetRef<ItemDef> weapon;
 *   ...
 *   if (const ItemDef* item = weapon.get())
 *     damage = item->value;
 */
template <typename T>
struct AssetRef : AssetRefBase
{
  /** The asset; null if none is set, it's missing, or it isn't a T. */
  const T* get() const;
  const T* operator->() const { return get(); }
  explicit operator bool() const { return get() != nullptr; }
};

/**
 * The project's (or game's) scriptable objects: .asset files, each holding the
 * field values of a scriptable object type ({ "formatVersion", "type", "fields"
 * }). Loaded from the catalogue on first use and kept until forgotten; one per
 * process, like the type registry, so game modules and the editor share it.
 *
 * Mesh fields name meshes in a scene's mesh library: whoever runs a scene sets
 * it (setMeshes), which starts the cache afresh. Objects of a game module's
 * types must be gone before it's unloaded: clear() first.
 */
class ScriptableObjects
{
public:
  static constexpr std::string_view kExtension = ".asset";

  // An asset found by list(): its reference ("asset:<id>"), name and type.
  struct Entry
  {
    std::string asset;
    std::string name; // its file's name, without the extension
    std::string type;
  };

  // An asset of type T, found by all<T>().
  template <typename T>
  struct Found
  {
    std::string asset;
    std::string name;
    const T* data = nullptr;
  };

  ScriptableObjects() = default;
  ~ScriptableObjects();
  ScriptableObjects(const ScriptableObjects&) = delete;
  ScriptableObjects& operator=(const ScriptableObjects&) = delete;

  /** Where assets are found (null: nowhere). Forgets everything loaded. */
  void setCatalogue(const AssetDatabase* catalogue);
  const AssetDatabase* catalogue() const { return m_catalogue; }
  /** The meshes Mesh fields are found in (null: none). Forgets everything
   * loaded, if it changes. */
  void setMeshes(MeshLibrary* meshes);

  /** An asset by reference ("asset:<id>"); null if none, missing or unreadable.
   */
  const ScriptableObject* get(std::string_view asset);
  template <typename T>
  const T* get(std::string_view asset)
  {
    return dynamic_cast<const T*>(get(asset));
  }

  /** The project's scriptable objects of a type (by its name; empty: every
   * type), in path order. */
  std::vector<Entry> list(std::string_view type = {});
  /** Every asset of type T, loaded, in path order. */
  template <typename T>
  std::vector<Found<T>> all()
  {
    std::vector<Found<T>> found;
    for (const Entry& entry : list())
      if (const T* data = get<T>(entry.asset))
        found.push_back({entry.asset, entry.name, data});
    return found;
  }

  /** Read an asset again next time it's asked for (its file changed). */
  void forget(std::string_view asset);
  /** Forget every loaded asset (e.g. before a game module is unloaded). */
  void clear();

  /** An .asset file's contents for `fields` of `type`. */
  static nlohmann::json fileContents(const std::string& type,
                                     const nlohmann::json& fields);

private:
  struct Loaded
  {
    std::string type;
    ScriptableObject* object = nullptr; // made by its type's ScriptableOps
    void (*destroy)(void*) = nullptr;
  };

  // The type a file holds, read from it once.
  const std::string& typeOf(const std::string& asset);

  const AssetDatabase* m_catalogue = nullptr;
  MeshLibrary* m_meshes = nullptr;
  std::map<std::string, Loaded, std::less<>>
      m_loaded; // by reference; object null: unreadable
  std::map<std::string, std::string, std::less<>>
      m_types; // each file's type, by reference
};

/** The process-wide scriptable objects. */
ScriptableObjects& scriptableObjects();

template <typename T>
const T* AssetRef<T>::get() const
{
  return asset.empty() ? nullptr : scriptableObjects().get<T>(asset);
}

} // namespace sfs
