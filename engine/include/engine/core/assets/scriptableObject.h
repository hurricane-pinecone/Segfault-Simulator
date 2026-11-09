#pragma once

#include <string>
#include <string_view>

namespace sfs
{

/**
 * The base of a scriptable object type: shared data kept in an asset file
 * (".asset") rather than on an entity, like Unity's ScriptableObjects. Declare
 * one in your scripts with editable fields, as for a component:
 *
 *   struct ItemDef : sfs::ScriptableObject
 *   {
 *     std::string displayName;
 *     int value = 10;
 *     sfs::MeshHandle model;
 *   };
 *
 * The editor's Assets panel makes them (New Scriptable Object), the Inspector
 * edits them, and anything refers to one with an sfs::AssetRef<ItemDef> field.
 * One asset is shared by everything that uses it. See sfs::ScriptableObjects.
 */
struct ScriptableObject
{
  virtual ~ScriptableObject() = default;
};

/**
 * What a reference to a scriptable object holds: the asset, as "asset:<id>"
 * (empty: none). Use the typed sfs::AssetRef<T>, which gets the asset.
 */
struct AssetRefBase
{
  std::string asset;

  bool empty() const { return asset.empty(); }
  bool operator==(const AssetRefBase&) const = default;
};

/**
 * The scriptable object an "asset:<id>" names (null: none, missing or
 * unreadable), through the resolver the runtime installs (the process-wide
 * sfs::ScriptableObjects; none installed: null).
 */
const ScriptableObject* findScriptableObject(std::string_view asset);
using ScriptableObjectResolver = const ScriptableObject* (*)(std::string_view);
void setScriptableObjectResolver(ScriptableObjectResolver resolver);

/**
 * A field referring to a scriptable object of type T (an
 * sfs::ScriptableObject). The editor picks it from the project's assets of
 * that type and of types derived from it; get() gives the asset, loaded the
 * first time anything asks for it and shared by everything that does.
 *
 *   sfs::AssetRef<ItemDef> weapon;
 *   ...
 *   if (const ItemDef* item = weapon.get())
 *     damage = item->value;
 *
 * T may be only declared where the field is (a component naming another
 * feature's asset type); it must be complete where get() is called.
 */
template <typename T>
struct AssetRef : AssetRefBase
{
  /** The asset; null if none is set, it's missing, or it isn't a T (an
   * asset of a type derived from T is one). */
  const T* get() const
  {
    return asset.empty() ? nullptr
                         : dynamic_cast<const T*>(findScriptableObject(asset));
  }
  const T* operator->() const { return get(); }
  explicit operator bool() const { return get() != nullptr; }
};

} // namespace sfs
