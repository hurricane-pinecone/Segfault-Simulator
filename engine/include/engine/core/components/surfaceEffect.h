#pragma once

namespace sfs
{

/**
 * A surface material for a tile or voxel block type (BlockType::effect):
 * liquids (water, lava; the voxel world's water columns) and terrain looks
 * (grass, sand, snow). None: a plain surface.
 *
 * @param SurfaceEffect::Type type - the material to apply
 */
struct SurfaceEffect
{
  enum class Type : int
  {
    None = 0,

    /* --- Liquids ---*/
    Water,
    Lava,

    /* --- Terrain ---*/
    Grass,
    Sand,
    Snow
  };

  SurfaceEffect(Type type) : type(type){};

  Type type = Type::None;
};

} // namespace sfs
