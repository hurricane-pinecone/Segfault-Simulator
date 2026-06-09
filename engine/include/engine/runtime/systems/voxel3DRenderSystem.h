#pragma once

#include "engine/core/ecs/system.h"
#include "engine/core/util/asyncJobQueue.h"
#include "engine/core/voxel/tinyVoxelMesher.h"
#include "engine/runtime/rendering/camera/orthoOrbitCamera.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "glm/glm/ext/vector_int3.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sfs
{

class TinyVoxelWorld;
class VoxelFireSystem;

// The 3D tiny-voxel renderer (NOT the iso quad/geometry renderer): draws the
// TinyVoxelWorld with a real MVP pipeline under an orthographic orbit camera.
// It reads the world (storage/streaming live there), caches a GPU mesh per
// loaded chunk (re-meshing the world's dirty chunks, freeing unloaded ones),
// and owns the camera, day/night sun, point lights, transparent water, the
// player marker, and the destruction (debris + tumbling blocks; edits route
// through the world).
class Voxel3DRenderSystem : public System
{
public:
  Voxel3DRenderSystem(int windowWidth, int windowHeight);
  ~Voxel3DRenderSystem();

  Voxel3DRenderSystem(const Voxel3DRenderSystem&) = delete;
  Voxel3DRenderSystem& operator=(const Voxel3DRenderSystem&) = delete;

  // The data authority this renderer draws (chunks, water, surface, edits).
  void setWorld(TinyVoxelWorld* world) { m_world = world; }

  // Fire source: the renderer emits + draws flame particles from its burning
  // voxels (the fire SIM lives in VoxelFireSystem; this is just the visuals).
  void setFire(const VoxelFireSystem* fire) { m_fire = fire; }

  OrthoOrbitCamera& camera() { return m_camera; }
  void setViewport(int width, int height);
  void setLightDir(const glm::vec3& dir) { m_lightDir = dir; }

  // Time-of-day sun: direction (toward sun), tint, ambient floor, and the
  // directional strength. Drives the day/night look.
  void setSun(const glm::vec3& dir,
              const glm::vec3& color,
              float ambient,
              float diffuse)
  {
    m_lightDir = dir;
    m_sunColor = color;
    m_ambient = ambient;
    m_sunDiffuse = diffuse;
  }

  // Point lights, in WORLD voxel space. Replaced each frame (cheap); the shader
  // sums up to 16. radius is the reach in voxels.
  struct PointLight
  {
    glm::vec3 pos{0.0f};
    glm::vec3 color{1.0f};
    float radius = 100.0f;
    float intensity = 1.0f;
  };
  void setLights(std::vector<PointLight> lights)
  {
    m_lights = std::move(lights);
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

  // Mouse pick: unproject NDC (-1..1, y up) to a world ray, march it, and
  // return the first solid voxel hit (false if the ray misses the terrain).
  bool raycastVoxel(const glm::vec2& ndc, glm::ivec3& outVoxel) const;

  // Destruction: carve a sphere (radius in voxels) at `center` (via the world),
  // flinging the removed voxels as a fine spray + rigid tumbling blocks.
  void explode(const glm::ivec3& center, int radius, float power);

  // One chunk's (or the player's) GPU buffers. Public so the upload helper in
  // the .cpp can fill it.
  struct GpuMesh
  {
    unsigned int vao = 0;
    unsigned int vbo = 0;
    int vertexCount = 0;
  };

  // A flying single voxel (the fine spray). Public so the mesh builder reads
  // it.
  struct Debris
  {
    glm::vec3 pos{0.0f};
    glm::vec3 vel{0.0f};
    glm::vec4 color{1.0f};
    float life = 0.0f;
  };

  // A rising flame puff emitted by a burning voxel. Public so the mesh builder
  // reads it.
  struct Flame
  {
    glm::vec3 pos{0.0f};
    glm::vec3 vel{0.0f};
    float life = 0.0f;
    float maxLife = 1.0f;
  };

  // A rigid block of voxels (a sizable chunk) that tumbles, bounces, rolls down
  // slopes, then bakes back into the world when it settles.
  struct Block
  {
    glm::vec3 pos{0.0f}; // centre, world voxels
    glm::vec3 vel{0.0f};
    glm::vec3 angle{0.0f}; // euler orientation (visual tumble)
    glm::vec3 angVel{0.0f};
    int size = 3;                      // cube edge in voxels
    std::vector<std::uint32_t> voxels; // size^3 colours (0 = empty)
    float restTimer = 0.0f;            // time spent slow on the ground
    float groundTime = 0.0f; // time spent in contact (hard settle fallback)
    bool dead = false;
    GpuMesh mesh; // local-space mesh built once; tumbled on the GPU via uModel
  };

protected:
  void update(double deltaTime) override; // wave clock + debris/block physics
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
  void
  syncMeshes(); // free unloaded chunk meshes, re-mesh the world's dirty ones
  void bakeDebris(const glm::ivec3& cell,
                  const glm::vec4& color); // settle a spray voxel
  void bakeBlock(const Block& block); // settle a rigid block (axis-aligned)

  // Per chunk: an opaque mesh (terrain) + a water mesh (translucent), so water
  // draws in a separate blended pass.
  struct ChunkGpu
  {
    GpuMesh opaque;
    GpuMesh water;
  };

  TinyVoxelWorld* m_world = nullptr;
  std::unordered_map<glm::ivec3, ChunkGpu, KeyHash> m_gpu;
  std::unordered_set<glm::ivec3, KeyHash>
      m_remeshPending; // dirty, awaiting a job

  // Async meshing: a chunk's mesh is built on background workers from an
  // immutable snapshot (the chunk + its 6 neighbour border layers) tagged with
  // the chunk's generation; finished meshes land in m_meshResults and the main
  // thread uploads a budget per frame, discarding any whose generation is stale
  // (re-edited since).
  struct MeshResult
  {
    glm::ivec3 coord{0};
    std::uint64_t gen = 0;
    TinyChunkMesh mesh; // opaque + water vertex lists
  };
  std::unordered_map<glm::ivec3, std::uint64_t, KeyHash> m_chunkGen;
  std::vector<MeshResult> m_meshResults;
  std::mutex m_meshResultsMutex;

  OrthoOrbitCamera m_camera;
  glm::vec3 m_lightDir{0.4f, 0.9f, 0.3f};
  glm::vec3 m_sunColor{1.0f, 1.0f, 1.0f};
  float m_ambient = 1.0f;
  float m_sunDiffuse = 0.6f;

  bool m_hasPlayer = false;
  glm::vec3 m_playerCenter{0.0f};
  glm::vec3 m_playerHalf{0.5f};
  glm::vec3 m_playerColor{0.9f, 0.2f, 0.2f};
  GpuMesh m_playerMesh;

  std::vector<PointLight> m_lights;

  std::vector<Debris> m_debris;
  GpuMesh m_debrisMesh;

  std::vector<Block> m_blocks;

  const VoxelFireSystem* m_fire = nullptr;
  std::vector<Flame> m_flames;
  GpuMesh m_flameMesh;
  GpuMesh m_emberMesh;
  std::uint32_t m_flameFrame = 0;

  unsigned int m_program = 0;
  int m_uViewProj = -1;
  int m_uModel = -1;
  int m_uEmissive = -1;
  int m_uLightDir = -1;
  int m_uSunColor = -1;
  int m_uAmbient = -1;
  int m_uSunDiffuse = -1;
  int m_uLightCount = -1;
  int m_uLightPos = -1;
  int m_uLightColor = -1;
  int m_uLightRadius = -1;
  int m_uLightIntensity = -1;
  bool m_initialized = false;

  int m_viewportW = 1;
  int m_viewportH = 1;

  AsyncJobQueue
      m_meshQueue; // LAST member: joins (drains jobs) first on teardown
};

} // namespace sfs
