#pragma once

#include "engine/core/noise/noise.h"
#include "engine/core/voxel/voxelChunk.h"
#include "engine/core/voxel/voxelView.h"
#include "engine/core/voxel/waterChunk.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/vector_int3.hpp"
#include "voxel/blockRegistry.h"
#include <cstdint>

// Sample world generation: solid ground up to a 2D noise surface (in ELEVATION
// LEVELS / half-blocks, so odd heights get a half-block slab cap). Water is a
// GLOBAL SEA LEVEL -- every column whose terrain dips below it fills to it.
// That makes water a pure per-column function, so it is perfectly seamless
// across streamed chunks (the neighbour computes the same level by definition)
// with no flood/fill pass. Caves are carved out of the solid; the sea only
// fills air ABOVE the terrain surface, so enclosed caves stay dry. Sand rings
// the waterline (bed + beach band); everything else is grass. High "crater"
// lakes and dry sub-sea pits will be deterministic, self-contained FEATURES
// layered on top of this base (each brings its own walls, so it can't seam).
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

    const int seaCells = kWaterLevelBlocks; // sea level in whole cells
    const int seaLevel = seaCells * L;      // sea level in elevation levels

    for (int ly = 0; ly < sfs::kChunkSize; ++ly)
      for (int lx = 0; lx < sfs::kChunkSize; ++lx)
      {
        const int wx = baseX + lx;
        const int wy = baseY + ly;
        const int levels = surfaceLevels(wx, wy);
        const int fullBlocks = levels / L; // whole cube cells
        const bool slab = (levels % L) != 0;
        const int solidCells = fullBlocks + (slab ? 1 : 0);
        const bool underwater = levels < seaLevel;
        const bool sandy =
            levels <= seaLevel + kShoreMargin; // bed + beach band

        const sfs::BlockId cube =
            sandy ? GameBlockRegistry::kSand : GameBlockRegistry::kGrass;
        const sfs::BlockId slabId = sandy ? GameBlockRegistry::kSandSlab
                                          : GameBlockRegistry::kGrassSlab;

        for (int lz = 0; lz < sfs::kChunkSize; ++lz)
        {
          const int wz = baseZ + lz;

          // Carve caves out of the solid, BELOW the top crust so the surface
          // (and any sea resting on it) stays intact -- caves show at cliffs or
          // when dug into, and an enclosed one stays dry (the sea never fills
          // inside the solid).
          if (wz < solidCells - 1 && carved(wx, wy, wz))
            continue;

          if (wz < fullBlocks)
          {
            out.set(lx, ly, lz, cube);
          }
          else if (wz == fullBlocks && slab)
          {
            out.set(lx, ly, lz, slabId);
            // A submerged slab holds water in its empty upper level, so the sea
            // meets the half block instead of leaving a gap.
            if (underwater)
              out.setWater(lx, ly, lz, sfs::kWaterFull / 2);
          }
          else if (underwater && wz >= solidCells && wz < seaCells)
          {
            out.setWater(lx, ly, lz, sfs::kWaterFull); // full cell below sea
          }
        }
      }
  }

  // Sea level in whole cells. Lower = less water (only the deeper valleys
  // flood).
  static constexpr int kWaterLevelBlocks = 5;

private:
  int surfaceLevels(int x, int y) const
  {
    const float n = m_noise.get(static_cast<float>(x), static_cast<float>(y));
    // Balanced [0,1] -- NO low-biasing square, so most land sits above the sea
    // and only genuine valleys dip below it (keeps lakes from covering the
    // map).
    const float t = (n + 1.0f) * 0.5f;
    constexpr int kMaxLevels = 22;
    // Floor at one full block so every column has a solid cube floor.
    return glm::max(sfs::kLevelsPerBlock,
                    static_cast<int>(t * static_cast<float>(kMaxLevels)));
  }

  // Pseudo-3D density from 2D noise: fold z into both sample planes so the
  // field varies on all three axes. Above the threshold = carved (air).
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
  static constexpr int kShoreMargin = 3; // sandy beach band above the waterline

  sfs::Noise m_noise;
  sfs::Noise m_caveNoise;
};
