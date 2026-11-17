#include "engine/runtime/rendering/modules/imageQuads.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/rendering/mesh.h"
#include "engine/core/rendering/meshPrimitives.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering/meshLibrary.h"

#include <glm/glm/ext/vector_float2.hpp>

#include <optional>

namespace sfs
{

namespace
{
constexpr const char* QUAD_NAME = "builtin:imageQuad";
}

MeshHandle imageQuad(MeshLibrary& meshes)
{
  if (const MeshHandle quad = meshes.resolve(QUAD_NAME); quad.valid())
    return quad;

  return meshes.add(meshes::plane(glm::vec2(1.0f)), QUAD_NAME);
}

MeshMaterial imageMaterial(AssetStore& assets,
                           const std::string& image,
                           const std::string& normalImage,
                           AlphaMode mode,
                           bool unlit)
{
  MeshMaterial material;
  material.baseTexture = assets.imageTexture(image);

  // Normals are data, not colour: a texture asset's are read as such.
  if (normalImage.starts_with("asset:"))
  {
    if (const std::optional<AssetId> id = AssetId::parse(normalImage.substr(6)))
      material.normalTexture = assets.dataTexture(*id);
  }
  else if (!normalImage.empty())
    material.normalTexture = assets.imageTexture(normalImage);

  material.roughness = 1.0f;
  material.alphaMode = mode;
  material.unlit = unlit;
  return material;
}

} // namespace sfs
