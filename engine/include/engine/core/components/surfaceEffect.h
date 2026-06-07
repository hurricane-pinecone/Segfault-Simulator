#pragma once

namespace sfs
{

/**
 * Tags a tile with a material the renderer styles specially: animated liquids
 * (water, lava) and terrain looks (grass, sand, snow). None draws the sprite
 * unchanged.
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
