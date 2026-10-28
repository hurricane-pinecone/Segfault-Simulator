#pragma once

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/reflection/reflection.h"

#include "nlohmann/json.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace sfs
{
class MeshLibrary;
}

// Scriptable object (.asset) files in a project: making one of a scriptable object type
// (with the type's defaults), and reading and writing its fields. See
// sfs::ScriptableObjects for the format.
namespace scriptableObjectFiles
{

/**
 * Write a new scriptable object of `type` into `folder` (relative to the project,
 * e.g. "assets/items"), named `name`.asset (with a number added if that's
 * taken), its fields the type's defaults.
 *
 * @return its id, or nothing (logged) if it can't be written
 */
std::optional<sfs::AssetId> create(sfs::AssetDatabase& assets,
                                   const std::filesystem::path& folder,
                                   const sfs::reflect::TypeInfo& type,
                                   const std::string& name,
                                   sfs::MeshLibrary* meshes);

/** A scriptable object file's contents ({ "type", "fields", ... }); nothing (logged) if it can't be read. */
std::optional<nlohmann::json> read(const sfs::AssetDatabase& assets, const sfs::AssetRecord& record);

/** Save a scriptable object's fields. False (logged) if it can't. */
bool write(const sfs::AssetDatabase& assets,
           const sfs::AssetRecord& record,
           const std::string& type,
           const nlohmann::json& fields);

} // namespace scriptableObjectFiles
