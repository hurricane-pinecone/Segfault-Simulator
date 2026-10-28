#pragma once

#include "engine/runtime/assetStore/modelData.h"
#include "nlohmann/json.hpp"

#include <filesystem>
#include <optional>

namespace sfs
{

/** How a model file is brought in; kept in its asset's .meta "settings". */
struct ModelImportSettings
{
  float scale = 1.0f; // multiplies every position (e.g. 0.01 for centimetres)

  static ModelImportSettings fromJson(const nlohmann::json& settings);
};

/**
 * Read a glTF model (.glb or .gltf) into engine meshes. glTF is Y up; models
 * come in Z up (the way Blender shows them), with node transforms baked into
 * the whole model. Each primitive becomes a mesh section with its material:
 * base colour, base colour and metallic-roughness textures (inside a .glb, or
 * image files beside a .gltf), metal, roughness and alpha mode. Normal,
 * occlusion and emissive maps are ignored.
 *
 * @return nothing (logged) if the file can't be read or has no triangles
 */
std::optional<ModelData> importModel(const std::filesystem::path& file,
                                     const ModelImportSettings& settings = {});

/**
 * Just a model's node tree (as ModelData::nodes), read from the file's
 * structure without loading its geometry or textures: quick, for placing a
 * model as a tree of entities whose meshes load separately.
 */
std::optional<std::vector<ModelNode>>
importModelNodes(const std::filesystem::path& file,
                 const ModelImportSettings& settings = {});

} // namespace sfs
