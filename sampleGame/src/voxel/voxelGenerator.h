#pragma once

#include "engine/core/noise/noise.h"
#include "engine/core/voxel/voxelChunk.h"
#include "engine/core/voxel/voxelView.h"
#include "engine/core/voxel/waterChunk.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/vector_int3.hpp"
#include "voxel/blockRegistry.h"

// Sample world generation: solid ground from the floor up to a 2D noise surface
// measured in ELEVATION LEVELS (half-blocks), so an odd height is capped with a
// half-block slab -- demonstrating both block sizes (including underwater).
// Sand at/under the shoreline, grass above. Water is filled as SETTLED per-cell
// amounts up to sea level (a flat sea over the seabed); the fluid sim leaves it
// alone until something disturbs it. Caves/overhangs (a 3D carve) are the next
// phase -- the engine already supports air anywhere, so only this file changes.
class GameVoxelGenerator : public sfs::IVoxelGenerator
{
public:
  GameVoxelGenerator()
  {
    m_noise.setSeed(1337);
    m_noise.setFrequency(0.035f);
    m_noise.setType(sfs::Noise::Type::OpenSimplex);

    // Caves: a separate, higher-frequency field carved out of the solid.
    m_caveNoise.setSeed(9001);
    m_caveNoise.setFrequency(0.09f);
    m_caveNoise.setType(sfs::Noise::Type::OpenSimplex);
  }

  void generate(glm::ivec3 chunkCoord, sfs::IChunkWriter& out) const override
  {
    const int L = sfs::kLevelsPerBlock;
    const int baseX = chunkCoord.x * sfs::kChunkSize;
    const int baseY = chunkCoord.y * sfs::kChunkSize;
    const int baseZ = chunkCoord.z * sfs::kChunkSize;

    for (int ly = 0; ly < sfs::kChunkSize; ++ly)
      for (int lx = 0; lx < sfs::kChunkSize; ++lx)
      {
        const int seaLevel = kWaterLevelBlocks * L; // sea surface in levels
        const int levels = surfaceLevels(baseX + lx, baseY + ly);
        const int fullBlocks = levels / L; // whole cube cells
        const bool slab = (levels % L) != 0;
        const int solidCells = fullBlocks + (slab ? 1 : 0);
        const bool underwater = levels < seaLevel;
        const bool beach = levels <= seaLevel;

        const sfs::BlockId cube =
            beach ? GameBlockRegistry::kSand : GameBlockRegistry::kGrass;
        const sfs::BlockId slabId = beach ? GameBlockRegistry::kSandSlab
                                          : GameBlockRegistry::kGrassSlab;

        for (int lz = 0; lz < sfs::kChunkSize; ++lz)
        {
          const int wz = baseZ + lz;
          const int wx = baseX + lx;
          const int wy = baseY + ly;

          // Carve caves out of the solid (not water). A cave that breaches the
          // surface or a cliff shows; a fully-enclosed one is invisible until
          // exposed -- exactly the voxel behaviour we want to prove.
          // TEMP: caves disabled to test half blocks in isolation.
          // if (wz <= fullBlocks && carved(wx, wy, wz))
          //   continue;
          (void)wx;
          (void)wy;

          if (wz < fullBlocks)
            out.set(lx, ly, lz, cube);
          else if (wz == fullBlocks && slab)
          {
            out.set(lx, ly, lz, slabId);
            // A submerged slab holds water in its empty upper level, so the sea
            // surface meets the half block instead of leaving a hole.
            if (underwater)
              out.setWater(lx, ly, lz, sfs::kWaterFull / 2);
          }
          else if (underwater && wz >= solidCells && wz < kWaterLevelBlocks)
            out.setWater(lx, ly, lz, sfs::kWaterFull); // full cell below sea
        }
      }
  }

  // Sea level in whole cells. Water depth = sea level - seabed, and the seabed
  // bottoms out at the bedrock floor (one cube), so this is the knob for how
  // deep the water gets: deepest water = (kWaterLevelBlocks - 1) cells.
  static constexpr int kWaterLevelBlocks =
      4; // ~3 blocks of water in the basins

private:
  int surfaceLevels(int x, int y) const
  {
    const float n = m_noise.get(static_cast<float>(x), static_cast<float>(y));
    float t = (n + 1.0f) * 0.5f;
    t = t * t;
    // Tall enough that plenty of land still rises above the (now higher) sea --
    // otherwise the low-biased noise floods most of the map.
    constexpr int kMaxLevels = 24;
    // Floor at one full block so every column (seabed included) has a solid
    // cube floor -- otherwise the deepest water sits over nothing and shows a
    // hole.
    return glm::max(sfs::kLevelsPerBlock,
                    static_cast<int>(t * static_cast<float>(kMaxLevels)));
  }

  // Pseudo-3D density from 2D noise: fold z into both sample planes so the
  // field varies on all three axes (a true 3D noise would be cleaner, but this
  // proves the concept). Above the threshold = carved (air).
  bool carved(int x, int y, int z) const
  {
    const float fz = static_cast<float>(z);
    const float a = m_caveNoise.get(
        static_cast<float>(x) + fz * 31.7f, static_cast<float>(y));
    const float b = m_caveNoise.get(
        static_cast<float>(x), static_cast<float>(y) + fz * 53.3f);
    return (a + b) * 0.5f > kCaveThreshold;
  }

  static constexpr float kCaveThreshold = 0.55f;

  sfs::Noise m_noise;
  sfs::Noise m_caveNoise;
};
