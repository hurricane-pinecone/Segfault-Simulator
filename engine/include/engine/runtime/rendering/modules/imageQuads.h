#pragma once

#include "engine/core/rendering/mesh.h"

#include <string>

namespace sfs
{

class AssetStore;
class MeshLibrary;

/**
 * The unit quad (1 x 1 in its XY plane, facing +Z, its image's top up +Y)
 * that sprites, particles and decals draw images with, one a library.
 */
MeshHandle imageQuad(MeshLibrary& meshes);

/**
 * A material showing an image added to the asset store by name (a sprite
 * sheet, a particle's texture), with its normal map if it has one: lit and
 * cut out unless `mode` and `unlit` say otherwise.
 */
MeshMaterial imageMaterial(AssetStore& assets,
                           const std::string& image,
                           const std::string& normalImage,
                           AlphaMode mode,
                           bool unlit = false);

} // namespace sfs
