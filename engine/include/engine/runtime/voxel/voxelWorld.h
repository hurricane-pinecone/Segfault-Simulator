#pragma once

#include "engine/core/ecs/system.h"
#include "engine/core/rendering/iTerrainSurfaceSource.h"
#include "engine/core/rendering/iWaterSurfaceSource.h"
#include "engine/core/voxel/voxelChunk.h"
#include "engine/core/voxel/voxelView.h"
#include "engine/core/voxel/waterCell.h"
#include "engine/core/voxel/waterChunk.h"
#include "engine/core/voxel/waterSim.h"
#include "glm/glm/ext/vector_int2.hpp"
#include "glm/glm/ext/vector_int3.hpp"
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace sfs
{

struct IVec3Hash
{
  std::size_t operator()(const glm::ivec3& v) const noexcept
  {
    const auto x = static_cast<std::uint32_t>(v.x);
    const auto y = static_cast<std::uint32_t>(v.y);
    const auto z = static_cast<std::uint32_t>(v.z);
    return static_cast<std::size_t>(x * 73856093u ^ y * 19349663u ^
                                    z * 83492791u);
  }
};

struct IVec2HashKey
{
  std::size_t operator()(const glm::ivec2& v) const noexcept
  {
    const auto x = static_cast<std::uint32_t>(v.x);
    const auto y = static_cast<std::uint32_t>(v.y);
    return static_cast<std::size_t>(x * 73856093u ^ y * 19349663u);
  }
};

// Engine voxel terrain store: a sparse 3D grid of block ids, streamed around
// the camera and filled by an injected IVoxelGenerator. This is the data
// authority queried by the renderer (the VoxelTerrain module reads it), by
// collision, and by lighting -- it is NOT itself a renderer. Coexists with the
// heightfield path; a game registers one or the other. Blocks are data here,
// not entities, so a screenful of terrain stays cheap.
class VoxelWorld : public System,
                   public IVoxelView,
                   public ITerrainSurfaceSource,
                   public IWaterSurfaceSource,
                   public IWaterGrid
{
public:
  // World height in BLOCK layers [minBlockZ, maxBlockZ); radiusTiles is the
  // streamed (x,y) half-extent around the camera.
  VoxelWorld(int minBlockZ, int maxBlockZ, int radiusTiles = 48);

  void setGenerator(const IVoxelGenerator* generator)
  {
    m_generator = generator;
  }

  // Needed to classify blocks (solid ground vs liquid) when tracking the
  // surface; without it every non-air block counts as solid ground.
  void setBlockRegistry(const IBlockRegistry* registry)
  {
    m_registry = registry;
  }

  // IVoxelView
  BlockId blockAt(int x, int y, int z) const override;

  // ITerrainHeightSource: the surface top in elevation levels (caves below it
  // don't change it), so lighting/decals/picking keep working unchanged.
  int terrainHeightAt(int tileX, int tileY) const override;

  // The nearest reachable floor for an actor at `fromLevel` (the surface, a
  // step, or a cave floor below ground), plus whether a wall/ceiling blocks it.
  // Lets actors walk through caves while cliffs still stop them.
  WalkableFloor walkableFloor(int tileX,
                              int tileY,
                              int fromLevel,
                              int maxClimb,
                              int clearance) const override;

  // ITerrainSurfaceSource: a column whose water surface sits above its solid
  // floor reads as water (so blood landing there fades like a puddle/lake).
  bool isWaterAt(int tileX, int tileY) const override;

  // IWaterSurfaceSource: every column with water above its floor, for the water
  // render module to draw the animated surface (the voxel counterpart to
  // WaterTileComponent entities).
  void collectWaterColumns(std::vector<WaterColumn>& out) const override;

  // Edit one block; marks its chunk (and the -x/-y/-z neighbours whose +faces
  // abut it) dirty so the renderer remeshes only what changed.
  void setBlock(int x, int y, int z, BlockId id);

  // Add water to a cell and wake the sim around it (a source / a splash / a
  // bucket). Clamped to the cell's free capacity.
  void addWater(int x, int y, int z, int amount);
  int waterAt(int x, int y, int z) const { return water({x, y, z}); }

  // IWaterGrid (the fluid sim reads/writes the world through this).
  int water(const glm::ivec3& cell) const override;
  int capacity(const glm::ivec3& cell) const override;
  float surface(const glm::ivec3& cell) const override;
  void setWater(const glm::ivec3& cell, int amount) override;

  // --- consumed by the VoxelTerrain render module ---
  bool hasChunk(const glm::ivec3& c) const { return m_chunks.count(c) != 0; }
  const std::unordered_map<glm::ivec3, std::unique_ptr<VoxelChunk>, IVec3Hash>&
  chunks() const
  {
    return m_chunks;
  }
  const std::unordered_set<glm::ivec3, IVec3Hash>& dirtyChunks() const
  {
    return m_dirty;
  }
  void clearDirty() { m_dirty.clear(); }

protected:
  void update(double deltaTime) override;

private:
  static int floorDiv(int a, int b);
  static glm::ivec3 chunkOf(int x, int y, int z);

  void streamAround(const glm::ivec2& focusTile);
  void loadChunk(const glm::ivec3& coord);
  void markDirty(const glm::ivec3& coord);

  bool isSolid(BlockId id) const;
  // The block's top surface in elevation levels at cell z (a slab tops out one
  // level up; a cube two).
  int topLevel(BlockId id, int z) const;
  // Rescan a column for its topmost solid and refresh m_surfaceTop -- needed
  // after an edit that could LOWER the surface (e.g. destroying the top block).
  void recomputeSurfaceTop(int x, int y);

  // Wake a cell + its neighbours for the next sim tick.
  void activate(const glm::ivec3& cell);
  // Advance the fluid sim on a fixed timestep (independent of streaming).
  void simulateWater(double deltaTime);
  // Refresh the topmost-water-cell-per-column cache after a cell changed.
  void updateWaterTop(const glm::ivec3& cell, int amount);

  int m_minChunkZ;
  int m_maxChunkZ;
  int m_radiusTiles;
  const IVoxelGenerator* m_generator = nullptr;
  const IBlockRegistry* m_registry = nullptr;

  std::unordered_map<glm::ivec3, std::unique_ptr<VoxelChunk>, IVec3Hash>
      m_chunks;
  std::unordered_set<glm::ivec3, IVec3Hash> m_dirty;
  // Top solid (walkable) surface in elevation levels per column, for
  // terrainHeightAt (already accounts for slab vs cube height).
  std::unordered_map<glm::ivec2, int, IVec2HashKey> m_surfaceTop;

  // --- Water: a per-cell amount layer (NOT blocks), simulated as a conserving
  // fluid. Surfaces can rest at any height (meeting half blocks). ---
  std::unordered_map<glm::ivec3, WaterChunk, IVec3Hash> m_waterChunks;
  std::unordered_set<glm::ivec3, IVec3Hash> m_activeWater; // cells to sim next
  // Topmost water-bearing cell z per column, for the render surface.
  std::unordered_map<glm::ivec2, int, IVec2HashKey> m_waterTopCell;
  double m_simAccum = 0.0;

  glm::ivec2 m_lastFocus;
  bool m_streamed = false;
};

} // namespace sfs
