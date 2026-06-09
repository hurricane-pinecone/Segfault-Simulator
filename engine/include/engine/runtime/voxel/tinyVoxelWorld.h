#pragma once

#include "engine/core/ecs/system.h"
#include "engine/core/util/asyncJobQueue.h"
#include "engine/core/voxel/tinyVoxelChunk.h"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include "glm/glm/ext/vector_int3.hpp"

#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sfs
{

class ITinyVoxelGenerator;

// Self-contained chunk/column key hashes (no dependency on the iso VoxelWorld).
struct TinyIVec3Hash
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
struct TinyIVec2Hash
{
  std::size_t operator()(const glm::ivec2& v) const noexcept
  {
    const auto x = static_cast<std::uint32_t>(v.x);
    const auto y = static_cast<std::uint32_t>(v.y);
    return static_cast<std::size_t>(x * 73856093u ^ y * 19349663u);
  }
};

// The streaming tiny-voxel world: the data authority (NOT a renderer). Owns the
// chunks loaded in a disc around a focus point (x,z effectively infinite; y a
// fixed chunk band), filled by an injected generator. Tracks the surface per
// column + the water columns, answers solidity/colour/surface queries, and
// applies edits (destruction routes through setVoxel). Mirrors the iso
// VoxelWorld's role for the 3D path.
class TinyVoxelWorld : public System
{
public:
  using ChunkMap =
      std::unordered_map<glm::ivec3, TinyVoxelChunk, TinyIVec3Hash>;
  using ChunkSet = std::unordered_set<glm::ivec3, TinyIVec3Hash>;

  TinyVoxelWorld(int minChunkY, int maxChunkY, int radiusChunks);

  void setGenerator(const ITinyVoxelGenerator* generator);
  void setFocus(const glm::vec3& worldPos) { m_focus = worldPos; }

  // Queries (world voxel coords).
  bool solidAt(int x, int y, int z) const;
  std::uint32_t voxelAt(int x, int y, int z) const;
  int surfaceTop(int x, int z) const; // top of the highest solid (+1), or floor
  bool hasColumn(int x, int z) const  // is this column streamed in (has a bed)?
  {
    return m_surfaceTop.find({x, z}) != m_surfaceTop.end();
  }
  int surfaceBelow(int x, int yStart, int z) const; // for debris/block landing
  int seaLevel() const { return m_seaLevel; }

  // Edit (carve/bake): updates the surface incrementally + marks chunks dirty.
  void setVoxel(int x, int y, int z, std::uint32_t color);

  // Renderer reads.
  const ChunkMap& chunks() const { return m_chunks; }
  const ChunkSet& dirtyChunks() const { return m_dirty; }
  void clearDirty() { m_dirty.clear(); }
  const ChunkSet& unloadedChunks() const { return m_unloaded; }
  void clearUnloaded() { m_unloaded.clear(); }

protected:
  void update(double deltaTime) override; // streams around the focus

private:
  // A freshly generated column, produced PURELY (no shared-state access) so a
  // pool of workers can generate columns in parallel; merged in serially after.
  struct ColumnData
  {
    int cx = 0;
    int cz = 0;
    std::vector<std::pair<glm::ivec3, TinyVoxelChunk>> chunks; // non-empty only
    std::vector<int> surface; // surfaceTop per (lx,lz), row-major 32x32
  };
  ColumnData generateColumn(int cx, int cz) const; // pure: gen + local surface
  void mergeColumn(ColumnData&& column);           // serial: insert + dirty

  void unloadColumn(int cx, int cz);
  void recomputeSurfaceTop(int x, int z);
  void markDirty(const glm::ivec3& chunkCoord);

  const ITinyVoxelGenerator* m_gen = nullptr;
  glm::vec3 m_focus{0.0f, 0.0f, 0.0f};
  glm::ivec2 m_lastFocusChunk{std::numeric_limits<int>::min(),
                              std::numeric_limits<int>::min()};

  int m_minChunkY = 0;
  int m_maxChunkY = 0;
  int m_radiusChunks = 6;
  int m_seaLevel = 0;

  ChunkMap m_chunks;
  ChunkSet m_dirty;
  ChunkSet m_unloaded; // chunks dropped this frame -> renderer frees their mesh
  std::unordered_map<glm::ivec2, int, TinyIVec2Hash> m_surfaceTop;
  std::unordered_set<glm::ivec2, TinyIVec2Hash> m_loadedColumns;

  // Async generation: column coords go to background workers (generateColumn is
  // pure); finished columns land in m_genResults and the main thread merges a
  // budget per frame. m_inFlight (main-thread only) dedups submissions.
  std::unordered_set<glm::ivec2, TinyIVec2Hash> m_inFlight;
  std::vector<ColumnData> m_genResults;
  mutable std::mutex m_genResultsMutex;
  AsyncJobQueue
      m_genQueue; // LAST member: joins (drains jobs) first on teardown
};

} // namespace sfs
