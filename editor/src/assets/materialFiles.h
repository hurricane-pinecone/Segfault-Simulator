#pragma once

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/rendering/material.h"
#include "engine/core/rendering/mesh.h"

#include <filesystem>
#include <optional>
#include <string>

// Material asset (.mat) files in a project's assets/: creating, reading and
// writing them, and extracting a model's material into one so it can be
// edited.
namespace materialFiles
{

/**
 * Write a new material asset into `folder` (relative to the project, e.g. "assets/materials"), named
 * `name`.mat (with a number added if that's taken).
 *
 * @return its id, or nothing (logged) if it can't be written
 */
std::optional<sfs::AssetId> create(sfs::AssetDatabase& assets,
                                   const std::filesystem::path& folder,
                                   const std::string& name,
                                   const sfs::MaterialAsset& material = {});

/**
 * A blank splat map (all the first layer) for a terrain material: a `size`
 * square PNG texture asset in `folder`, named `name`.
 *
 * @return its name ("asset:<id>"), or empty (logged) if it can't be written
 */
std::string createSplatMap(sfs::AssetDatabase& assets,
                           const std::filesystem::path& folder,
                           const std::string& name,
                           int size = 1024);

/** A material asset's contents; nothing (logged) if it can't be read. */
std::optional<sfs::MaterialAsset> read(const sfs::AssetDatabase& assets, const sfs::AssetRecord& record);

/** Save a material asset's contents. False (logged) if it can't. */
bool write(const sfs::AssetDatabase& assets, const sfs::AssetRecord& record, const sfs::MaterialAsset& material);

/**
 * Turn a mesh's own material into a material asset in assets/materials/:
 * its textures saved beside it as PNGs, named after `name` (e.g.
 * "barn_Roof").
 *
 * @return the new material's id, or nothing (logged)
 */
std::optional<sfs::AssetId> extract(sfs::AssetDatabase& assets, const sfs::MeshMaterial& material, const std::string& name);

} // namespace materialFiles
