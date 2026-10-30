#include "engine/runtime/rendering/modules/imageQuads.h"

#include "engine/core/rendering/meshPrimitives.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering/meshLibrary.h"

namespace sfs
{

namespace
{
constexpr const char* kQuadName = "builtin:imageQuad";
}

MeshHandle imageQuad(MeshLibrary& meshes)
{
  if (const MeshHandle quad = meshes.resolve(kQuadName); quad.valid())
    return quad;
  return meshes.add(meshes::plane(glm::vec2(1.0f)), kQuadName);
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
