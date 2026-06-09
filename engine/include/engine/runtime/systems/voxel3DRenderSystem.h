#pragma once

#include "engine/core/ecs/system.h"
#include "engine/core/voxel/tinyVoxelChunk.h"
#include "engine/runtime/rendering/camera/orthoOrbitCamera.h"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_int3.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sfs
{

// A self-contained 3D render path (NOT the iso quad/geometry renderer): draws
// tiny coloured voxels with a real MVP pipeline under an orthographic orbit
// camera. Lives as a plain System -- the scene hands it chunks + drives the
// camera; it owns its GL program, per-chunk buffers, and GL state. Coexists
// with the iso renderer but is meant to be used alone in a 3D scene.
class Voxel3DRenderSystem : public System
{
public:
  Voxel3DRenderSystem(int windowWidth, int windowHeight);
  ~Voxel3DRenderSystem();

  Voxel3DRenderSystem(const Voxel3DRenderSystem&) = delete;
  Voxel3DRenderSystem& operator=(const Voxel3DRenderSystem&) = delete;

  // Insert/replace a chunk's voxels; marks it (and its neighbours) for
  // remeshing.
  void setChunk(const glm::ivec3& chunkCoord, const TinyVoxelChunk& chunk);

  OrthoOrbitCamera& camera() { return m_camera; }
  void setViewport(int width, int height);
  void setLightDir(const glm::vec3& dir) { m_lightDir = dir; }

  // One column of water: the floor it sits on and the surface (sea) level, both
  // world voxel Y. The surface is re-meshed each frame with animated waves and
  // drawn transparent, so you see the bed through it.
  struct WaterColumn
  {
    int x = 0;
    int z = 0;
    int floorY = 0;
  };
  void setWater(std::vector<WaterColumn> columns, int seaLevel)
  {
    m_water = std::move(columns);
    m_seaLevel = seaLevel;
  }

  // Optional player marker: a solid box drawn each frame (centre +
  // half-extents).
  void setPlayerBox(const glm::vec3& center,
                    const glm::vec3& halfExtents,
                    const glm::vec3& color)
  {
    m_playerCenter = center;
    m_playerHalf = halfExtents;
    m_playerColor = color;
    m_hasPlayer = true;
  }

  // Sample solidity in WORLD voxel coords across the loaded chunks.
  bool solidWorld(int wx, int wy, int wz) const;

  // One chunk's (or the player's) GPU buffers. Public so the upload helper in
  // the .cpp can fill it.
  struct GpuMesh
  {
    unsigned int vao = 0;
    unsigned int vbo = 0;
    int vertexCount = 0;
  };

protected:
  void update(double deltaTime) override; // advances the wave clock
  void render() override;

private:
  struct KeyHash
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

  void ensureInitialized();
  void remeshDirty();

  std::unordered_map<glm::ivec3, TinyVoxelChunk, KeyHash> m_chunks;
  std::unordered_set<glm::ivec3, KeyHash> m_dirty;
  std::unordered_map<glm::ivec3, GpuMesh, KeyHash> m_gpu;

  OrthoOrbitCamera m_camera;
  glm::vec3 m_lightDir{0.4f, 0.9f, 0.3f};

  bool m_hasPlayer = false;
  glm::vec3 m_playerCenter{0.0f};
  glm::vec3 m_playerHalf{0.5f};
  glm::vec3 m_playerColor{0.9f, 0.2f, 0.2f};
  GpuMesh m_playerMesh;

  std::vector<WaterColumn> m_water;
  int m_seaLevel = 0;
  GpuMesh m_waterMesh;
  double m_time = 0.0; // wave clock

  unsigned int m_program = 0;
  int m_uViewProj = -1;
  int m_uLightDir = -1;
  bool m_initialized = false;

  int m_viewportW = 1;
  int m_viewportH = 1;
};

} // namespace sfs
