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
  static constexpr int kBodyDim = 96; // detached rigid-body / fell-window grid
                                      // edge (== BODYDIM in voxelCommon.wgsl);
                                      // big enough to hold a whole tree
  static constexpr int kMaxBodies =
      256; // rigid-body pool size (== MAXB in voxelCommon.wgsl). Logical slots;
           // physical storage is size-classed (below).

  // Body voxel pool. Single 96^3 class (size classes reverted -- the
  // multi-class allocator had a "tree doesn't fall" bug still to debug).
  static constexpr uint64_t kBodyPoolBytes = static_cast<uint64_t>(kMaxBodies) *
                                             kBodyDim * kBodyDim * kBodyDim *
                                             sizeof(uint32_t);
  static constexpr uint64_t kClassFreeU32 = kMaxBodies + 1;

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

  // Debug: the cursor pixel, highlighted by the render to show what it hits.
  void setDebugMouse(float x, float y)
  {
    m_dbgMouseX = x;
    m_dbgMouseY = y;
  }

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
  void buildBodyShed();
  void buildBodyStep();
  void buildBodyXform();
  void buildBodyPlace();
  void buildBodyPlaceStorage();
  void buildBodyFreeStorage();
  void buildBodyReap();
  void buildBodyPrep();
  void buildBodyLabel();
  void buildBodySplit();
  void buildWindowAnchor();
  void buildWindowSplit();

  WebGpuContext& m_ctx;
  WGPUDevice m_device = nullptr;
  WGPUQueue m_queue = nullptr;

  uint64_t m_voxBytes = 0;
  uint64_t m_brickBytes = 0;
  uint64_t m_faceBytes = 0;
  uint64_t m_anchorBytes = 0;
  WGPUBuffer m_voxBuf[2] = {nullptr, nullptr};
  WGPUBuffer m_brickBuf = nullptr;
  WGPUBuffer m_materialBuf =
      nullptr; // palette: colour/density/... per material id
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
  uint64_t m_bodyBytes = 0;       // one slot's grid
  WGPUBuffer m_bodyBuf = nullptr; // kMaxBodies stacked grids
  // Size-class body storage (Stage B/C2): each slot's voxels live in a pool
  // block sized to its class; a descriptor holds the (offset, dim), blocks come
  // from a free-list. A small body costs ~its class size instead of a full
  // 96^3.
  WGPUBuffer m_bodyDescBuf = nullptr;  // kMaxBodies * (offset, dim)
  WGPUBuffer m_classFreeBuf = nullptr; // [0]=free count, [1..]=free block ids
  WGPUBuffer m_bodyXformBuf = nullptr; // array<Body, kMaxBodies>
  WGPUBuffer m_slotMetaBuf = nullptr;  // per-slot AABB/count/CoM/footprint
  WGPUBuffer m_slotMetaReadback = nullptr; // mapped to place the bodies
  WGPUBuffer m_rootSlotBuf = nullptr;      // per-brick component-root -> slot
  WGPUBuffer m_slotCountBuf = nullptr;     // atomic split-root ordering counter
  WGPUBuffer m_freeSlotBuf = nullptr; // [0]=free count, [1..]=free slot ids
  WGPUBuffer m_slotOccupiedBuf =
      nullptr; // per-slot atomic occupancy (GPU owns
               // allocation; register passes claim it)
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
  // GPU-resident body motion: integration + transform matrices run as compute
  // passes over m_bodyStateBuf (4 vec4/slot), so the CPU no longer integrates
  // or builds the transforms each frame.
  WGPUBuffer m_bodyStateBuf = nullptr;
  WGPUBuffer m_bodyStateReadback =
      nullptr;                         // status for stamp/free + split capture
  WGPUBuffer m_bodyStepUBuf = nullptr; // dt uniform
  WGPUComputePipeline m_bodyStepPipe = nullptr;
  WGPUComputePipeline m_bodyXformPipe = nullptr;
  WGPUBindGroup m_bodyStepBg = nullptr;
  WGPUBindGroup m_bodyXformBg = nullptr;
  // Break-off: hard-impact body voxels shed into the world as rubble (powder).
  WGPUComputePipeline m_bodyShedPipe = nullptr;
  WGPUBindGroup m_bodyShedBg[2] = {nullptr, nullptr}; // per src buffer
  // GPU placement: reduced slot metadata -> initial motion state (replaces the
  // CPU onSlotMetaMapped math; split children read the parent's live state).
  WGPUComputePipeline m_bodyPlacePipe = nullptr;
  WGPUBindGroup m_bodyPlaceBg = nullptr;
  WGPUComputePipeline m_bodyPlaceStoragePipe =
      nullptr; // size-class block alloc
  WGPUBindGroup m_bodyPlaceStorageBg = nullptr;
  WGPUComputePipeline m_bodyFreeStoragePipe = nullptr; // returns blocks on bake
  WGPUBindGroup m_bodyFreeStorageBg = nullptr;
  WGPUBuffer m_freeReqBuf = nullptr; // [0]=count, [1..]=slots freed this frame
  // GPU reap: frees baked slots (block + occupancy + flag) each frame,
  // replacing the CPU resting loop.
  WGPUComputePipeline m_bodyReapPipe = nullptr;
  WGPUBindGroup m_bodyReapBg = nullptr;
  WGPUBuffer m_placeUBuf = nullptr; // (isSplit, parentSlot)
  // GPU-driven per-frame body dispatch sizing (replaces the CPU mirror's
  // activeBound). The prep pass scans state flags -> activeHigh, writes the
  // indirect dispatch args + the render's dbgMouse.z bound.
  WGPUComputePipeline m_bodyPrepPipe = nullptr;
  WGPUBindGroup m_bodyPrepBg = nullptr;
  WGPUBuffer m_bodyArgsBuf = nullptr; // indirect dispatch args (x,y,z)
  float m_stepDt = 0.016f;
  bool m_bodyStateMapBusy = false;
  bool m_bodyStatePendingResolve = false;
  // Carving a falling body + splitting it: which slot the carve hit (read
  // back), a voxel-level component label of that body's grid, and a recolor
  // debug pass.
  WGPUBuffer m_carveHitBuf = nullptr; // [0]=body hit? [1]=slot
  WGPUBuffer m_carveHitReadback = nullptr;
  WGPUBuffer m_bodyLabelBuf[2] = {nullptr,
                                  nullptr}; // ping-pong, one slot's grid
  WGPUBuffer m_bodyLabelSlotBuf = nullptr;  // uniform: slot to label
  WGPUComputePipeline m_bodyLabelInitPipe = nullptr;
  WGPUComputePipeline m_bodyLabelFloodPipe = nullptr;
  WGPUComputePipeline m_bodyRecolorPipe = nullptr;
  WGPUBindGroup m_bodyLabelInitBg = nullptr;
  WGPUBindGroup m_bodyLabelFloodBg[2] = {nullptr, nullptr};
  WGPUBindGroup m_bodyRecolorBg = nullptr;
  // Split the carved body's components into pool slots (voxel-level analogue of
  // the world fell). Reuses rootSlot/slotMeta/freeSlot/slotCount + the label
  // slot uniform (= parent slot).
  WGPUComputePipeline m_bodySplitRegisterPipe = nullptr;
  WGPUComputePipeline m_bodySplitReducePipe = nullptr;
  WGPUComputePipeline m_bodySplitFootprintPipe = nullptr;
  WGPUComputePipeline m_bodySplitExtractPipe = nullptr;
  WGPUComputePipeline m_bodySplitClearParentPipe = nullptr;
  WGPUBindGroup m_bodySplitBg = nullptr;
  // Voxel-exact world fell: a DIM^3 window around the carve, ground-anchored by
  // a voxel flood seeded off the coarse-anchored bulk beyond the box
  // (ping-pong).
  WGPUBuffer m_windowBuf[2] = {nullptr, nullptr};
  WGPUComputePipeline m_winInitPipe = nullptr;
  WGPUComputePipeline m_winFloodPipe = nullptr;
  WGPUComputePipeline m_winMarkPipe = nullptr;
  WGPUBindGroup m_winBg[2] = {nullptr, nullptr};
  // Voxel-exact world fell stage 2/3: CC + extract over the window's detached
  // set (reuses m_windowBuf as the label ping-pong,
  // m_rootSlot/m_slotMeta/m_bodyBuf).
  WGPUComputePipeline m_winLabelInitPipe = nullptr;
  WGPUComputePipeline m_winLabelFloodPipe = nullptr;
  WGPUComputePipeline m_winRegisterPipe = nullptr;
  WGPUComputePipeline m_winReducePipe = nullptr;
  WGPUComputePipeline m_winExtractPipe = nullptr;
  WGPUBindGroup m_winLabelInitBg = nullptr;
  WGPUBindGroup m_winLabelFloodBg[2] = {nullptr, nullptr};
  WGPUBindGroup m_winRegisterBg = nullptr;
  WGPUBindGroup m_winReduceBg = nullptr;
  WGPUBindGroup m_winExtractBg[2] = {nullptr, nullptr}; // [srcIdx]: vox0 = src
  bool m_pendingIsSplit = false; // the pending slotMeta readback is a split
  float m_splitParentCenter[3] = {0.0f, 0.0f, 0.0f};
  float m_splitParentPivot[3] = {0.0f, 0.0f, 0.0f};
  float m_splitParentVelY = 0.0f;
  WGPUBuffer m_dbgMouseBuf = nullptr;
  float m_dbgMouseX = -100.0f;
  float m_dbgMouseY = -100.0f;
  int m_carvedSlot = -1; // body slot the last carve hit, else -1
  bool m_carveMapBusy = false;
  bool m_carvePendingResolve = false;
  struct CarveHitRb
  {
    WGPUBuffer buffer;
    int* slot;
    bool* busy;
  } m_carveRb{};

  RigidBody m_bodies[kMaxBodies];
  bool m_fellRequested =
      false; // carving this frame -> auto-fell what it severed
  int m_fellHold =
      0; // frames to process the full slot range after a carve (new
         // bodies are GPU-active before the CPU mirror catches up)
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
  // Mirrors the GPU motion state back to the CPU (one frame late) only for what
  // the CPU still owns: resting (stamp/free gating) and the split parent's live
  // center/velY. Not authoritative for active (placement/free are CPU-driven).
  struct BodyStateRb
  {
    WGPUBuffer buffer;
    RigidBody* bodies;
    bool* busy;
  } m_bodyStateRb{};

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
