#pragma once

#include "engine/webgpu/webGpuContext.h"

#include <webgpu/webgpu.h>

#include <cstdint>

namespace sfs
{

// The GPU-resident brickmap voxel world: two ping-pong voxel buffers + a brick
// occupancy grid, the five compute/render pipelines that drive it (generate,
// water CA, occupancy recount, click edit, raymarch render), and an optional
// GPU timestamp readback. Owns all the WebGPU resources; records a whole
// simulation
// + render frame into a caller-supplied command encoder.
//
// Static terrain lives permanently in both voxel buffers and is never cleared
// or copied; only dynamic water is ping-ponged, so the per-frame cost tracks
// the water volume, not the world size.
class GpuVoxelWorld
{
public:
  static constexpr int kWorld = 512;
  static constexpr int kBrick = 8;
  static constexpr int kBG = kWorld / kBrick; // 64
  static constexpr int kBodyDim = 64;         // detached rigid-body grid edge
  static constexpr int kMaxBodies =
      32; // rigid-body pool size (== MAXB in voxelCommon.wgsl)

  struct Brick
  {
    uint32_t occupancy;
    uint32_t pointer;
  };

  // CPU-side per-slot rigid body state (one falling chunk per pool slot).
  struct RigidBody
  {
    bool active = false;
    bool wasActive = false; // edge-detect activation to seed the sim
    bool landed = false;    // false = falling straight down, true = toppling
    bool collided = false;  // overlaps world solid (from the collide readback)
    bool resting = false;   // reached the flat pose; stamp pending
    float center[3] = {0.0f, 0.0f, 0.0f}; // footprint pivot (world)
    float pivot[3] = {0.0f, 0.0f, 0.0f};  // footprint pivot (body-local)
    float axis[3] = {0.0f, 0.0f, 1.0f};   // horizontal topple axis (toward CoM)
    float theta = 0.0f;                   // topple angle about the axis
    float omega = 0.0f;                   // topple rate
    float velY = 0.0f;                    // straight-down fall velocity
  };

  // A click edit: carve (mode 1) or spawn water (mode 2) a sphere where the ray
  // from `origin` along `dir` first hits solid.
  struct EditCmd
  {
    float origin[3];
    float dir[3];
    int mode;
    float radius;
  };

  explicit GpuVoxelWorld(WebGpuContext& ctx);
  ~GpuVoxelWorld();

  GpuVoxelWorld(const GpuVoxelWorld&) = delete;
  GpuVoxelWorld& operator=(const GpuVoxelWorld&) = delete;

  // One-time world generation (terrain + lakes + trees). Submits its own
  // command buffer.
  void generate();

  // Record one frame into `enc`: optional edit, water tick, occupancy recount,
  // then the raymarch render pass to `view`. `cam16` is the packed camera
  // uniform (4x vec4); `edit` is null when nothing is held.
  void recordFrame(WGPUCommandEncoder enc,
                   WGPUTextureView view,
                   const float* cam16,
                   const EditCmd* edit,
                   uint32_t frame);

  bool hasTimestamps() const { return m_hasTs; }
  double gpuSimMs() const { return m_gpuSimMs; }
  double gpuRenderMs() const { return m_gpuRenderMs; }
  double gpuTotalMs() const { return m_gpuTotalMs; }

  // Advance the active falling bodies (gravity + topple), once per frame.
  void stepBody(double dt);

private:
  void buildGenerate();
  void buildWater();
  void buildRecount();
  void buildEdit();
  void buildRender();
  void buildTimestamps();
  void buildFaces();
  void buildAnchor();
  void buildRefine();
  void buildLabel();
  void buildBodyRegister();
  void buildBodyReduce();
  void buildBodyExtract();
  void buildBodyCollide();
  void buildBodyStamp();

  WebGpuContext& m_ctx;
  WGPUDevice m_device = nullptr;
  WGPUQueue m_queue = nullptr;

  uint64_t m_voxBytes = 0;
  uint64_t m_brickBytes = 0;
  uint64_t m_faceBytes = 0;
  uint64_t m_anchorBytes = 0;
  WGPUBuffer m_voxBuf[2] = {nullptr, nullptr};
  WGPUBuffer m_brickBuf = nullptr;
  WGPUBuffer m_camBuf = nullptr;
  WGPUBuffer m_frameBuf = nullptr;
  WGPUBuffer m_editBuf = nullptr;
  // Connectivity: per-brick face masks, ping-pong ground-anchoring labels, and
  // a tiny "did the flood change anything" control buffer.
  WGPUBuffer m_faceBuf = nullptr;
  WGPUBuffer m_anchorBuf[2] = {nullptr, nullptr};
  WGPUBuffer m_floodCtlBuf = nullptr;
  // Per-brick "a carve touched this brick" flag, marked by the edit pass and
  // consumed by the voxel-refinement pass.
  WGPUBuffer m_dirtyBuf = nullptr;
  // Connected-component labels of the detached set (ping-pong; result in [0]).
  WGPUBuffer m_labelBuf[2] = {nullptr, nullptr};
  // Detached rigid bodies: a pool of kMaxBodies kBodyDim^3 local voxel grids in
  // one buffer (slot s at offset s*kBodyDim^3), composited by the render. The
  // labeled components are compacted into slots, one falling chunk per slot.
  uint64_t m_bodyBytes = 0;                // one slot's grid
  WGPUBuffer m_bodyBuf = nullptr;          // kMaxBodies stacked grids
  WGPUBuffer m_bodyXformBuf = nullptr;     // array<Body, kMaxBodies>
  WGPUBuffer m_slotMetaBuf = nullptr;      // per-slot AABB/count/CoM/footprint
  WGPUBuffer m_slotMetaReadback = nullptr; // mapped to place the bodies
  WGPUBuffer m_rootSlotBuf = nullptr;      // per-brick component-root -> slot
  WGPUBuffer m_slotCountBuf = nullptr;     // atomic slot allocator
  WGPUBuffer m_freeSlotBuf = nullptr; // [0]=free count, [1..]=free slot ids
  WGPUComputePipeline m_bodyRegisterPipe = nullptr;
  WGPUComputePipeline m_bodyReducePipe = nullptr;
  WGPUComputePipeline m_bodyExtractPipe = nullptr;
  WGPUBindGroup m_bodyRegisterBg = nullptr;
  WGPUBindGroup m_bodyReduceBg = nullptr;
  WGPUBindGroup m_bodyExtractBg = nullptr;
  // Per-frame body-vs-world collision: one landing flag per slot.
  WGPUBuffer m_collideBuf = nullptr;
  WGPUBuffer m_collideReadback = nullptr;
  WGPUComputePipeline m_bodyCollidePipe = nullptr;
  WGPUBindGroup m_bodyCollideBg = nullptr;
  WGPUComputePipeline m_bodyStampPipe = nullptr;
  WGPUBindGroup m_bodyStampBg = nullptr;

  RigidBody m_bodies[kMaxBodies];
  bool m_fellRequested =
      false; // carving this frame -> auto-fell what it severed
  bool m_bodyMapBusy = false;
  bool m_bodyPendingResolve = false;
  bool m_collideMapBusy = false;
  bool m_collidePendingResolve = false;
  struct SlotMetaRb
  {
    WGPUBuffer buffer;
    RigidBody* bodies;
    bool* busy;
  } m_slotRb{};
  struct CollideRb
  {
    WGPUBuffer buffer;
    RigidBody* bodies;
    bool* busy;
  } m_collideRb{};

  WGPUComputePipeline m_genPipe = nullptr;
  WGPUComputePipeline m_waterPipe = nullptr;
  WGPUComputePipeline m_recPipe = nullptr;
  WGPUComputePipeline m_editPipe = nullptr;
  WGPURenderPipeline m_renderPipe = nullptr;
  WGPUComputePipeline m_facesPipe = nullptr;
  WGPUComputePipeline m_seedPipe = nullptr;
  WGPUComputePipeline m_floodPipe = nullptr;
  WGPUComputePipeline m_refinePipe = nullptr;
  WGPUComputePipeline m_labelInitPipe = nullptr;
  WGPUComputePipeline m_labelFloodPipe = nullptr;

  WGPUBindGroup m_genBg = nullptr;
  WGPUBindGroup m_waterBg[2] = {nullptr, nullptr};
  WGPUBindGroup m_recBg[2] = {nullptr, nullptr};
  WGPUBindGroup m_editBg[2] = {nullptr, nullptr};
  WGPUBindGroup m_renderBg[2] = {nullptr, nullptr};
  WGPUBindGroup m_facesBg = nullptr;
  WGPUBindGroup m_seedBg = nullptr;
  WGPUBindGroup m_floodBg[2] = {nullptr, nullptr};
  WGPUBindGroup m_refineBg = nullptr;
  WGPUBindGroup m_labelInitBg = nullptr;
  WGPUBindGroup m_labelFloodBg[2] = {nullptr, nullptr};

  int m_srcIdx = 0;

  // GPU timestamp readback (benchmark HUD): water begin/end + render begin/end,
  // resolved one tick and read back the next so a copy never targets the mapped
  // buffer.
  bool m_hasTs = false;
  WGPUQuerySet m_tsQuery = nullptr;
  WGPUBuffer m_tsResolve = nullptr;
  WGPUBuffer m_tsReadback = nullptr;
  double m_gpuSimMs = 0.0;
  double m_gpuRenderMs = 0.0;
  double m_gpuTotalMs = 0.0;
  bool m_tsMapBusy = false;
  bool m_tsPendingResolve = false;

  struct TsReadback
  {
    WGPUBuffer buffer;
    double* simMs;
    double* renderMs;
    double* totalMs;
    bool* busy;
  } m_tsRb{};
};

} // namespace sfs
