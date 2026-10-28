#pragma once

#include <string>

namespace sfs
{

/**
 * The base of a scriptable object type: shared data kept in an asset file (".asset")
 * rather than on an entity, like Unity's ScriptableObjects. Declare one in your
 * scripts with editable fields, as for a component:
 *
 *   struct ItemDef : sfs::ScriptableObject
 *   {
 *     std::string displayName;
 *     int value = 10;
 *     sfs::MeshHandle model;
 *   };
 *
 * The editor's Assets panel makes them (New Scriptable Object), the Inspector edits
 * them, and anything refers to one with an sfs::AssetRef<ItemDef> field. One
 * asset is shared by everything that uses it. See sfs::ScriptableObjects.
 */
struct ScriptableObject
{
  virtual ~ScriptableObject() = default;
};

/**
 * What a reference to a scriptable object holds: the asset, as "asset:<id>" (empty:
 * none). Use the typed sfs::AssetRef<T>, which gets the asset.
 */
struct AssetRefBase
{
  std::string asset;

  bool empty() const { return asset.empty(); }
  bool operator==(const AssetRefBase&) const = default;
};

} // namespace sfs
