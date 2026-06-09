#pragma once

#include <array>
#include <cstdint>

namespace sfs
{

// Cubic chunk edge in TINY voxels. These are Teardown-style small coloured
// cubes (a separate path from the block/BlockId voxels): appearance is a flat
// per-voxel colour, no textures, no block ids.
constexpr int kTinyChunkSize = 32;

// A dense cube of tiny voxels. 0 = air; any non-zero value is a solid voxel
// whose colour is packed 0xRRGGBBAA (alpha 0xFF marks solid, so even pure black
// is non-zero). Built by tinyColor().
struct TinyVoxelChunk
{
  std::array<std::uint32_t, kTinyChunkSize * kTinyChunkSize * kTinyChunkSize>
      voxels{}; // all air

  static int index(int lx, int ly, int lz)
  {
    return (lz * kTinyChunkSize + ly) * kTinyChunkSize + lx;
  }

  std::uint32_t at(int lx, int ly, int lz) const
  {
    return voxels[index(lx, ly, lz)];
  }
  void set(int lx, int ly, int lz, std::uint32_t color)
  {
    voxels[index(lx, ly, lz)] = color;
  }
  bool solid(int lx, int ly, int lz) const { return at(lx, ly, lz) != 0u; }
  bool empty() const
  {
    for (std::uint32_t v : voxels)
      if (v != 0u)
        return false;
    return true;
  }
};

// Pack 8-bit channels into a solid voxel colour (alpha forced to 0xFF).
inline std::uint32_t tinyColor(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
  return (static_cast<std::uint32_t>(r) << 24) |
         (static_cast<std::uint32_t>(g) << 16) |
         (static_cast<std::uint32_t>(b) << 8) | 0xFFu;
}

} // namespace sfs
