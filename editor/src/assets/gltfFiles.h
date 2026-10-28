#pragma once

#include "engine/core/assets/assetDatabase.h"

#include <filesystem>

// A .gltf model keeps its geometry in a .bin beside it (and often its textures
// in image files), naming them by relative path. The Assets panel shows the
// model alone, and keeps those paths working as it's moved, renamed or
// deleted.
namespace gltfFiles
{

/** The .bin a .gltf keeps its data in: beside it, with its name. Null if none. */
const sfs::AssetRecord* companionOf(const sfs::AssetDatabase& assets, const sfs::AssetRecord& gltf);

/** Whether an asset is a .gltf's .bin (so not shown on its own). */
bool isCompanion(const sfs::AssetDatabase& assets, const sfs::AssetRecord& record);

/**
 * Move or rename an asset (as AssetDatabase::move). A .gltf takes its .bin
 * along (renamed to match), and the paths it names its data and textures by
 * are updated to where they now are from it.
 */
bool move(sfs::AssetDatabase& assets, const sfs::AssetId& id, const std::filesystem::path& relative);

/** Delete an asset (as AssetDatabase::remove); a .gltf's .bin goes with it. */
bool remove(sfs::AssetDatabase& assets, const sfs::AssetId& id);

} // namespace gltfFiles
