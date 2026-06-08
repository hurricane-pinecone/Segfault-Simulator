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

    // Caves carve a vertical air span below the surface crust. Winding tunnels
    // are the intersection of two fields (both near zero -> a tube); rooms are
    // a rare cheese field; the floor undulates; entrances rarely breach the
    // surface. All 2D fields (the world has no 3D noise).
    const auto setup = [](sfs::Noise& n, int seed, float freq)
    {
      n.setSeed(seed);
      n.setFrequency(freq);
      n.setType(sfs::Noise::Type::OpenSimplex);
    };
    setup(m_tunnelA, 9001, 0.055f);
    setup(m_tunnelB, 4242, 0.055f);
    setup(m_caveNoise, 7777, 0.045f);     // room cheese
    setup(m_caveFloor, 3131, 0.050f);     // floor undulation
    setup(m_entranceNoise, 8088, 0.025f); // sinkhole bowls
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
        const float fx = static_cast<float>(wx);
        const float fy = static_cast<float>(wy);

        const int baseLevels = surfaceLevels(wx, wy);
        const int baseFull = baseLevels / L; // original rock depth (for caves)

        // Sinkhole bowl: smoothly lower the surface where the entrance field is
        // high (above sea only, clamped to sea so it can't flood). The bowl
        // follows the noise gradient, so its walls stay gentle enough to walk
        // in and out of, and it digs straight down into the caves below.
        int sink = 0;
        if (baseLevels > seaLevel)
        {
          const float ent = m_entranceNoise.get(fx, fy);
          if (ent > kEntranceThreshold)
            sink =
                static_cast<int>((ent - kEntranceThreshold) * kEntranceDepth);
        }
        const int levels =
            sink > 0 ? glm::max(seaLevel, baseLevels - sink) : baseLevels;
        const int fullBlocks = levels / L; // whole cube cells (bowled surface)
        const bool slab = (levels % L) != 0;
        const int solidCells = fullBlocks + (slab ? 1 : 0);
        const bool underwater = levels < seaLevel;
        const bool sandy =
            levels <= seaLevel + kShoreMargin; // bed + beach band

        const sfs::BlockId cube =
            sandy ? GameBlockRegistry::kSand : GameBlockRegistry::kGrass;
        const sfs::BlockId slabId = sandy ? GameBlockRegistry::kSandSlab
                                          : GameBlockRegistry::kGrassSlab;

        // Caves are carved relative to the ORIGINAL rock depth (baseFull), so a
        // sinkhole bowl digs straight down into them. Winding tunnels (two
        // fields both near zero -> a tube) open into the occasional room.
        bool caveColumn = false;
        int caveBotCell = 0;
        int caveTopCell = -1;
        if (baseFull >= kCaveMinDepth)
        {
          const bool tunnel = glm::abs(m_tunnelA.get(fx, fy)) < kTunnelBand &&
                              glm::abs(m_tunnelB.get(fx, fy)) < kTunnelBand;
          const bool room = m_caveNoise.get(fx, fy) > kRoomThreshold;
          if (tunnel || room)
          {
            const float fl = (m_caveFloor.get(fx, fy) + 1.0f) * 0.5f; // [0,1]
            const int span = room ? kRoomCells : kTunnelCells;
            caveTopCell = baseFull - 2 - static_cast<int>(fl * kFloorRange);
            caveBotCell = glm::max(1, caveTopCell - span + 1);
            caveColumn = caveTopCell >= caveBotCell;
          }
        }

        for (int lz = 0; lz < sfs::kChunkSize; ++lz)
        {
          const int wz = baseZ + lz;

          // Carve the cave's air span out of the solid. Where a sinkhole bowl
          // has lowered the surface past it, the column is already open and the
          // cave just continues it -- that's the walk-in entrance. An enclosed
          // cave stays dry (the sea only fills air above the surface).
          if (caveColumn && wz >= caveBotCell && wz <= caveTopCell)
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

  static constexpr int kShoreMargin = 3; // sandy beach band above the waterline

  // Cave shape dials. A column needs this many cubes to fit a cave + crust.
  static constexpr int kCaveMinDepth = 4;
  static constexpr float kTunnelBand = 0.28f;    // corridor half-width (tubes)
  static constexpr float kRoomThreshold = 0.58f; // lower = more, bigger rooms
  static constexpr int kTunnelCells = 2;         // tunnel air height (cells)
  static constexpr int kRoomCells = 4;           // room air height (cells)
  static constexpr int kFloorRange = 2;          // floor undulation (cells)
  // Sinkhole bowls: lower threshold = more openings; depth scales the bowl so
  // it reaches the caves (peak sink ~ (1 - threshold) * depth levels).
  static constexpr float kEntranceThreshold = 0.58f;
  static constexpr float kEntranceDepth = 30.0f;

  sfs::Noise m_noise;
  sfs::Noise m_tunnelA;
  sfs::Noise m_tunnelB;
  sfs::Noise m_caveNoise;
  sfs::Noise m_caveFloor;
  sfs::Noise m_entranceNoise;
};
