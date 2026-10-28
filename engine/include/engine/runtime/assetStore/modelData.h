#pragma once

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/rendering/3d/mesh.h"
#include "glm/glm/ext/matrix_float4x4.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfs
{

/** One mesh of a model file, on its own (in its own space, not placed). */
struct ModelPart
{
  std::string name;
  MeshData mesh;
};

/**
 * One node of a model's tree, as the file arranges it: placed relative to its
 * parent (already in the engine's axes and import scale), drawing one of the
 * model's parts or nothing.
 */
struct ModelNode
{
  std::string name;
  int parent = -1;             // index into ModelData::nodes; -1: a top node
  glm::mat4 local{1.0f};       // relative to its parent
  std::string part;            // the ModelPart it draws; empty: none
};

/**
 * A model asset's geometry, converted to the engine's conventions (Z up,
 * counter-clockwise, linear colours): the whole model as its file places it,
 * plus each of its meshes on its own when it has more than one, and then its
 * node tree (parents before children), so it can be placed as a tree of
 * entities (e.g. to turn one part, like a windmill's sails).
 */
struct ModelData
{
  MeshData whole;
  std::vector<ModelPart> parts;
  std::vector<ModelNode> nodes; // only with parts
};

/**
 * The MeshLibrary name a model asset's mesh is known by, e.g.
 * "asset:63caefd5..." for the whole model, or "asset:63caefd5...:Door" for one
 * of its parts.
 * Saved scenes store this name, so the reference survives the file being
 * renamed or moved.
 */
std::string assetMeshName(const AssetId& model, std::string_view part = {});

struct AssetMeshName
{
  AssetId model;
  std::string part; // empty: the whole model
};

/** Read an assetMeshName(); nothing if `name` isn't one. */
std::optional<AssetMeshName> parseAssetMeshName(std::string_view name);

} // namespace sfs
