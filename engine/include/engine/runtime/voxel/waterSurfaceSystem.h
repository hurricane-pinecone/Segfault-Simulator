#pragma once

#include "engine/runtime/voxel/tinyVoxelWorld.h" // hashes + the world

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace sfs
{

// Volumetric voxel water simulated on a COARSE grid (one water cell per block =
// kCoarse voxels). Cellular leveling spreads by diffusion, whose time grows
// with distance SQUARED; at fine voxel scale a carved hole is so many cells
// wide that it levels ~256x slower than the iso game's block-scale sim.
// Simulating water at block scale restores that speed -- a hole fills in a few
// steps -- while staying volumetric (3D coarse cells), so carved
// overhangs/tunnels still hold water. The surface is block-chunky by design.
// Water flows DOWN then SPREADS to level, conserving volume; settled water is
// inactive (free); a per-frame work budget caps re-level cost. The grid is
// separate from terrain, so flowing water never re-meshes a terrain chunk.
// (kCoarse is keyed to the block size = kVPB = 16.)
class WaterSurfaceSystem : public System
{
public:
  explicit WaterSurfaceSystem(TinyVoxelWorld* world) : m_world(world) {}

  void setSeaLevel(int sea) { m_sea = sea; }
  int seaLevel() const { return m_sea; }

  // Edit hook: wake the water in/above a carved region (voxel center + radius)
  // so it starts flowing.
  void wake(const glm::ivec3& centerVoxel, int radiusVoxel);

  // Renderer surface queries (per VOXEL column).
  bool hasWater(int x, int z) const;
  float surfaceAt(int x, int z) const; // world voxel-Y of the top water surface

  // Tiled rebuild: the renderer re-meshes a 32-voxel tile when its water (or
  // terrain) changed.
  static glm::ivec2 tileOf(int x, int z);
  bool tileDirty(int tx, int tz) const
  {
    return m_dirtyTiles.count({tx, tz}) > 0;
  }
  void clearTile(int tx, int tz) { m_dirtyTiles.erase({tx, tz}); }
  void touchTile(int tx, int tz) { m_dirtyTiles.insert({tx, tz}); }

protected:
  void update(double deltaTime) override;

private:
  // Coarse-cell ops (coordinates are in coarse cells, not voxels).
  int waterAt(const glm::ivec3& c) const;
  int capacityAt(const glm::ivec3& c) const;     // 0 = solid/unloaded barrier
  float surfaceLevel(const glm::ivec3& c) const; // coarse-cell units
  void setWaterAt(const glm::ivec3& c, int amount);

  void step(int budget);
  void seedNewChunks(int budget);
  void releaseUnloaded();
  int topWaterCoarse(int cx,
                     int cz) const; // top coarse cell-Y with water, or INT_MIN
  bool
  coarseLevel(int cx, int cz, float& outVoxelY) const; // water surface voxel-Y
  // The water level for a FINE column: the coarse level here, or extended from
  // a watered coarse neighbour (so a shore cell next to the lake gets the
  // lake's level). The renderer compares this to the fine terrain bed to carve
  // a 1-voxel waterline out of the coarse sim.
  float fineLevel(int x, int z, bool& found) const;

  TinyVoxelWorld* m_world = nullptr;
  int m_sea = 0;
  double m_accum = 0.0;

  std::unordered_map<glm::ivec3, std::uint16_t, TinyIVec3Hash> m_water;
  std::unordered_set<glm::ivec3, TinyIVec3Hash> m_active;
  std::unordered_set<glm::ivec3, TinyIVec3Hash> m_seededChunks;
  std::unordered_set<glm::ivec2, TinyIVec2Hash> m_dirtyTiles;
};

} // namespace sfs
