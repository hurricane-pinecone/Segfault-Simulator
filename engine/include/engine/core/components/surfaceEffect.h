#pragma once

namespace sfs
{

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

  SurfaceEffect(Type type) : type(type) {};

  Type type = Type::None;
};

} // namespace sfs
