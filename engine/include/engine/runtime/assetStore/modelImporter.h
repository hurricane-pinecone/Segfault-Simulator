#pragma once

#include "engine/runtime/assetStore/modelData.h"
#include "nlohmann/json.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

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
 * Skins and animations come in too: a skinned mesh's part carries its skin
 * (MeshData::skin: its joints by node name, their inverse bind matrices) and
 * its vertices' first four joints and weights (JOINTS_0/WEIGHTS_0,
 * normalised); the file's animations become ModelData::animations, their
 * translation, rotation and scale channels (linear, step or cubic spline)
 * moved into the engine's axes. Morph targets (and their weight channels)
 * aren't read.
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

/**
 * Whether a model node (a Blender object) is named as a collider: its name
 * ends in "-collider", in any case, before any ".001"-style number Blender
 * adds to a copy ("Boom-collider", "boom-Collider.002"). Its node then places
 * as an entity with a BoxCollider3DComponent fitted to its mesh's bounds, in
 * the node's own space (so turned and scaled with it), and its mesh isn't
 * drawn: it's left out of the whole model and of the parts
 * (ModelNode::collider).
 */
bool isColliderNodeName(std::string_view name);

} // namespace sfs
