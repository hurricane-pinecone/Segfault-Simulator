#pragma once

#include <cstdint>

namespace sfs
{

// A tiny voxel packs its material id in the LOW byte (RRGGBBMM); 0 == air, so
// any non-zero voxel is solid. The RGB bytes are the rendered colour (the
// mesher ignores the material byte), the material byte drives simulation
// (flammability, later: fluids, reactions). Keep terrain/structures tagged so
// fire/etc. know what they're touching.
enum class TinyMaterial : std::uint8_t
{
  Air = 0,
  Grass = 1,
  Dirt = 2,
  Stone = 3,
  Sand = 4,
  Wood = 5,
  Leaves = 6,
  Water = 7,     // translucent fluid voxel (meshed in the transparent pass)
  Generic = 255, // tagged by tinyColor(): solid, no special behaviour
};

// Pack an RGB colour + material into a voxel value.
inline std::uint32_t
tinyVoxel(std::uint8_t r, std::uint8_t g, std::uint8_t b, TinyMaterial material)
{
  return (static_cast<std::uint32_t>(r) << 24) |
         (static_cast<std::uint32_t>(g) << 16) |
         (static_cast<std::uint32_t>(b) << 8) |
         static_cast<std::uint32_t>(static_cast<std::uint8_t>(material));
}

inline TinyMaterial tinyMaterialOf(std::uint32_t voxel)
{
  return static_cast<TinyMaterial>(voxel & 0xFFu);
}

inline bool tinyFlammable(TinyMaterial m)
{
  return m == TinyMaterial::Wood || m == TinyMaterial::Leaves;
}

inline bool tinyIsWater(TinyMaterial m) { return m == TinyMaterial::Water; }

// Rendering class of a voxel: 0 = air, 1 = water (translucent), 2 = opaque
// solid. Drives face culling: opaque faces show against air OR water; water
// faces show against air only.
enum class TinyClass : int
{
  Air = 0,
  Water = 1,
  Opaque = 2,
};
inline TinyClass tinyClassOf(std::uint32_t voxel)
{
  if (voxel == 0u)
    return TinyClass::Air;
  return tinyIsWater(tinyMaterialOf(voxel)) ? TinyClass::Water
                                            : TinyClass::Opaque;
}

} // namespace sfs
