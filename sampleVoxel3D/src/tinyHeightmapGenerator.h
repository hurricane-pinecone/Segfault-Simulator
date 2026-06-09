#pragma once

#include "engine/core/noise/noise.h"
#include "engine/core/voxel/iTinyVoxelGenerator.h"
#include "engine/core/voxel/tinyVoxelChunk.h"
#include "engine/core/voxel/tinyVoxelMaterial.h"
#include "glm/glm/common.hpp"

#include <algorithm>
#include <cstdint>

// The game's world generator: big rolling hills (broad macro noise + fine
// sub-block detail), stone/dirt/grass/sand bands, sea level for lakes, and
// trees (wood trunk + leaf canopy) scattered on grass. Voxels carry a material
// id (low byte) so the sim knows wood/leaves are flammable. Pure: same coord ->
// same voxel, so it streams; trees are stamped per-chunk from a deterministic
// grid.
class TinyHeightmapGenerator : public sfs::ITinyVoxelGenerator
{
public:
  TinyHeightmapGenerator()
  {
    m_noise.setSeed(1337);
    // Frequencies are in VOXEL space, so key them off kVPB to keep the feature
    // size constant in BLOCKS as the resolution changes. Lower = broader,
    // gentler slopes (rolling foothills); pair with a small kRangeBlocks for
    // low relief.
    m_noise.setFrequency(0.03f / kVPB); // broad, gentle hills (~33-block)
    m_noise.setType(sfs::Noise::Type::OpenSimplex);

    m_detail.setSeed(99);
    m_detail.setFrequency(1.44f / kVPB); // fine sub-block surface bumps
    m_detail.setType(sfs::Noise::Type::OpenSimplex);
  }

  // Voxels per old block -- THE resolution/scale knob. Everything keyed off it.
  static constexpr int kVPB = 16;

  void generate(glm::ivec3 chunkCoord, sfs::TinyVoxelChunk& out) const override
  {
    const int ox = chunkCoord.x * sfs::kTinyChunkSize;
    const int oy = chunkCoord.y * sfs::kTinyChunkSize;
    const int oz = chunkCoord.z * sfs::kTinyChunkSize;
    for (int lz = 0; lz < sfs::kTinyChunkSize; ++lz)
      for (int ly = 0; ly < sfs::kTinyChunkSize; ++ly)
        for (int lx = 0; lx < sfs::kTinyChunkSize; ++lx)
        {
          const std::uint32_t c = voxelAt(ox + lx, oy + ly, oz + lz);
          if (c != 0u)
            out.set(lx, ly, lz, c);
        }
    stampTrees(ox, oy, oz, out);
  }

  int seaLevel() const override { return kSeaBlocks * kVPB; }

  // Surface (top solid voxel Y) at a column -- used by the game for
  // spawn/placement without waiting for chunks to stream in.
  int terrainHeight(int wx, int wz) const
  {
    const float macro =
        m_noise.get(static_cast<float>(wx), static_cast<float>(wz));
    const float fine =
        m_detail.get(static_cast<float>(wx), static_cast<float>(wz));
    const float hBlocks =
        static_cast<float>(kBaseBlocks) + (macro + 1.0f) * 0.5f * kRangeBlocks;
    return static_cast<int>(hBlocks * static_cast<float>(kVPB) +
                            fine * kFineAmp);
  }

private:
  static constexpr int kBaseBlocks = 2;
  static constexpr int kRangeBlocks =
      8; // low relief -> rolling foothills (~10)
  static constexpr int kSeaBlocks = 5;
  static constexpr float kFineAmp = kVPB / 4.0f; // sub-block jitter (in voxels)
  static constexpr int kCrust = kVPB / 4;        // grass/sand cap thickness

  // Trees, sized + spaced in BLOCKS (keyed off kVPB). They DON'T need touching
  // canopies -- embers carry fire across the gaps, so they read as individual
  // trees on the terrain.
  static constexpr int kTreeCell = kVPB * 7 / 2; // ~3.5 blocks between trees
  static constexpr int kCanopyR = kVPB * 5 / 8; // leaf-ball radius (~1.25 wide)
  static constexpr int kTrunkHalf = kVPB / 8;   // trunk thickness

  static int floorDiv(int a, int b)
  {
    int q = a / b;
    int r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
      --q;
    return q;
  }

  static std::uint32_t hash2(int x, int z)
  {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^
                      static_cast<std::uint32_t>(z) * 19349663u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return h;
  }

  static float hash3(int x, int y, int z)
  {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^
                      static_cast<std::uint32_t>(y) * 19349663u ^
                      static_cast<std::uint32_t>(z) * 83492791u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
  }

  // Per-voxel brightness jitter + material id, so a flat surface still reads as
  // individual voxels and the sim can tell what each voxel is made of.
  static std::uint32_t
  shade(int r, int g, int b, int wx, int wy, int wz, sfs::TinyMaterial material)
  {
    const float k = 0.86f + 0.22f * hash3(wx, wy, wz);
    const auto ch = [&](int c)
    {
      return static_cast<std::uint8_t>(
          glm::clamp(static_cast<int>(static_cast<float>(c) * k), 0, 255));
    };
    return sfs::tinyVoxel(ch(r), ch(g), ch(b), material);
  }

  std::uint32_t voxelAt(int wx, int wy, int wz) const
  {
    const int h = terrainHeight(wx, wz);
    const int sea = kSeaBlocks * kVPB;
    if (wy < h)
    {
      if (wy >= h - kCrust)
        return h <= sea + kVPB
                   ? shade(206, 192, 142, wx, wy, wz, sfs::TinyMaterial::Sand)
                   : shade(86, 168, 80, wx, wy, wz, sfs::TinyMaterial::Grass);
      if (wy >= h - 2 * kVPB)
        return shade(122, 92, 60, wx, wy, wz, sfs::TinyMaterial::Dirt);
      return shade(108, 110, 124, wx, wy, wz, sfs::TinyMaterial::Stone);
    }
    if (wy < sea)
    {
      // Physical water: real voxels filling the air below sea level, darker
      // with depth. Meshed translucent (the bed shows through) in the water
      // pass.
      const float d =
          glm::clamp(static_cast<float>(sea - wy) / (6.0f * kVPB), 0.0f, 1.0f);
      const auto lerp = [&](int a, int b)
      { return static_cast<std::uint8_t>(a + static_cast<float>(b - a) * d); };
      return sfs::tinyVoxel(lerp(107, 26),
                            lerp(178, 71),
                            lerp(219, 148),
                            sfs::TinyMaterial::Water);
    }
    return 0u; // air
  }

  // Stamp the parts of any nearby trees that fall inside this chunk.
  void stampTrees(int ox, int oy, int oz, sfs::TinyVoxelChunk& out) const
  {
    constexpr int N = sfs::kTinyChunkSize;
    const int gx0 = floorDiv(ox - kCanopyR, kTreeCell);
    const int gx1 = floorDiv(ox + N - 1 + kCanopyR, kTreeCell);
    const int gz0 = floorDiv(oz - kCanopyR, kTreeCell);
    const int gz1 = floorDiv(oz + N - 1 + kCanopyR, kTreeCell);

    for (int gz = gz0; gz <= gz1; ++gz)
      for (int gx = gx0; gx <= gx1; ++gx)
      {
        const std::uint32_t h = hash2(gx, gz);
        if ((h % 2u) != 0u)
          continue; // ~1/2 of cells grow a tree
        // Jitter near the cell centre (+-cell/4) so spacing stays even.
        const int tx = gx * kTreeCell + kTreeCell / 4 +
                       static_cast<int>(h % (kTreeCell / 2));
        const int tz = gz * kTreeCell + kTreeCell / 4 +
                       static_cast<int>((h >> 10) % (kTreeCell / 2));
        const int surf = terrainHeight(tx, tz);
        if (surf <= seaLevel() + kVPB)
          continue; // grass only (skip sandy shores + water)
        const int trunkH = kVPB * 3 + static_cast<int>((h >> 20) % (kVPB * 2));
        const int topY = surf + trunkH;

        // Trunk: a thick column of wood from the surface up.
        for (int wy = glm::max(surf, oy); wy < glm::min(topY, oy + N); ++wy)
          for (int wz = glm::max(tz - kTrunkHalf, oz);
               wz <= glm::min(tz + kTrunkHalf, oz + N - 1);
               ++wz)
            for (int wx = glm::max(tx - kTrunkHalf, ox);
                 wx <= glm::min(tx + kTrunkHalf, ox + N - 1);
                 ++wx)
              out.set(wx - ox,
                      wy - oy,
                      wz - oz,
                      shade(101, 67, 33, wx, wy, wz, sfs::TinyMaterial::Wood));

        // Canopy: a leaf ball at the top (only the in-chunk part is iterated).
        const int r2 = kCanopyR * kCanopyR;
        for (int wy = glm::max(topY - kCanopyR, oy);
             wy <= glm::min(topY + kCanopyR, oy + N - 1);
             ++wy)
          for (int wz = glm::max(tz - kCanopyR, oz);
               wz <= glm::min(tz + kCanopyR, oz + N - 1);
               ++wz)
            for (int wx = glm::max(tx - kCanopyR, ox);
                 wx <= glm::min(tx + kCanopyR, ox + N - 1);
                 ++wx)
            {
              const int dx = wx - tx;
              const int dy = wy - topY;
              const int dz = wz - tz;
              if (dx * dx + dy * dy + dz * dz <= r2)
                out.set(
                    wx - ox,
                    wy - oy,
                    wz - oz,
                    shade(54, 110, 48, wx, wy, wz, sfs::TinyMaterial::Leaves));
            }
      }
  }

  sfs::Noise m_noise;
  sfs::Noise m_detail;
};
