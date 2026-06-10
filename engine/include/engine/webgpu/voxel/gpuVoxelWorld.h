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

  struct Brick
  {
    uint32_t occupancy;
    uint32_t pointer;
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

  // Extract the current detached set into a rigid body on the next frame (no-op
  // while a body is already active).
  void requestFell() { m_fellRequested = true; }

  // Advance the active falling body (gravity + tumble), once per frame.
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
  // Detached rigid body: a kBodyDim^3 local voxel grid + a transform uniform
  // the render composites against the world.
  uint64_t m_bodyBytes = 0;
  WGPUBuffer m_bodyBuf = nullptr;
  WGPUBuffer m_bodyXformBuf = nullptr;
  WGPUBuffer m_bodyMetaBuf = nullptr;      // detached AABB + count (GPU reduce)
  WGPUBuffer m_bodyMetaReadback = nullptr; // mapped to read the body centre
  WGPUComputePipeline m_bodyReducePipe = nullptr;
  WGPUComputePipeline m_bodyExtractPipe = nullptr;
  WGPUBindGroup m_bodyReduceBg = nullptr;
  WGPUBindGroup m_bodyExtractBg = nullptr;
  // Per-frame body-vs-world collision: a flag the falling body sets on landing.
  WGPUBuffer m_collideBuf = nullptr;
  WGPUBuffer m_collideReadback = nullptr;
  WGPUComputePipeline m_bodyCollidePipe = nullptr;
  WGPUBindGroup m_bodyCollideBg = nullptr;
  WGPUComputePipeline m_bodyStampPipe = nullptr;
  WGPUBindGroup m_bodyStampBg = nullptr;

  // CPU-side body state: drives the transform uniform + the extract trigger.
  bool m_fellRequested = false;
  bool m_bodyActive = false;
  float m_bodyCenter[3] = {0.0f, 0.0f, 0.0f}; // base pivot in the world
  float m_bodyPivot[3] = {0.0f, 0.0f, 0.0f};  // base pivot in body-local
  float m_bodyTheta = 0.0f;                   // topple angle about +z
  float m_bodyOmega = 0.0f;                   // topple rate
  float m_bodyVelY = 0.0f;                    // straight-down fall velocity
  bool m_bodyWasActive = false; // edge-detect activation to seed the sim
  bool m_bodyLanded = false;    // false = falling, true = toppling/resting
  bool m_bodyCollided = false;  // body overlaps world solid (collide readback)
  bool m_bodyMapBusy = false;
  bool m_bodyPendingResolve = false;
  bool m_collideMapBusy = false;
  bool m_collidePendingResolve = false;
  struct CollideRb
  {
    WGPUBuffer buffer;
    bool* collided;
    bool* busy;
  } m_collideRb{};
  struct BodyMetaRb
  {
    WGPUBuffer buffer;
    bool* active;
    float* center;
    float* pivot;
    bool* busy;
  } m_bodyRb{};

  WGPUComputePipeline m_genPipe = nullptr;
  WGPUComputePipeline m_waterPipe = nullptr;
  WGPUComputePipeline m_recPipe = nullptr;
  WGPUComputePipeline m_editPipe = nullptr;
  WGPURenderPipeline m_renderPipe = nullptr;
  WGPUComputePipeline m_facesPipe = nullptr;
  WGPUComputePipeline m_seedPipe = nullptr;
  WGPUComputePipeline m_floodPipe = nullptr;
  WGPUComputePipeline m_refinePipe = nullptr;

  WGPUBindGroup m_genBg = nullptr;
  WGPUBindGroup m_waterBg[2] = {nullptr, nullptr};
  WGPUBindGroup m_recBg[2] = {nullptr, nullptr};
  WGPUBindGroup m_editBg[2] = {nullptr, nullptr};
  WGPUBindGroup m_renderBg[2] = {nullptr, nullptr};
  WGPUBindGroup m_facesBg = nullptr;
  WGPUBindGroup m_seedBg = nullptr;
  WGPUBindGroup m_floodBg[2] = {nullptr, nullptr};
  WGPUBindGroup m_refineBg = nullptr;

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
