#pragma once

#include "engine/runtime/assetStore/modelData.h"
#include "nlohmann/json.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
 *
 * @param namePaths when given, gets each node's name path (what its stable id
 *                  is made from, see stableNodeId()), in the nodes' order
 */
std::optional<std::vector<ModelNode>>
importModelNodes(const std::filesystem::path& file,
                 const ModelImportSettings& settings = {},
                 std::vector<std::string>* namePaths = nullptr);

/**
 * A model node's stable id (ModelNode::id): a hash of its name path, with bit
 * 31 set so it never meets the small ids a prefab gives its own entities.
 *
 * The name path is the node's glTF names (decoded, so the same however the
 * exporter escaped them) from the model's top node down to it, joined with
 * "/" (the model's own root, its file, isn't part of it). A
 * "/", "#" or "\" inside a name is escaped with a "\". The k-th sibling
 * (from 1, in the file's order) with the same name as an earlier one is
 * "name#k"; an unnamed node's name is empty, numbered the same way. So
 * reordering nodes changes no id, and renaming one changes its id and its
 * descendants'. Two paths of one model whose hashes collide are told apart at
 * import: the later one in the file takes the next free id (logged).
 *
 * @param namePath the node's name path
 * @return its id, before any collision is resolved
 */
uint32_t stableNodeId(std::string_view namePath);

/**
 * A node's name as a segment of a name path (see stableNodeId()): the name
 * with "/", "#" and "\" escaped with a "\". It doesn't add the "#k" a
 * repeated sibling name gets.
 *
 * @param name the node's name (empty for an unnamed node)
 * @return the segment
 */
std::string nodeNameSegment(std::string_view name);

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
