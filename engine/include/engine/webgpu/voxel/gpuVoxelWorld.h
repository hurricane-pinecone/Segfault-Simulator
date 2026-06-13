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
           // physical storage is the sparse brickmap (below).

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

  // An explosion at the first solid hit along the ray from `origin` along
  // `dir`: craters a `radius` sphere, fells + flings the surroundings, and
  // (later) sprays ballistic debris. Queue one with queueExplosion(); processed
  // next recordFrame.
  struct BlastCmd
  {
    float origin[3];
    float dir[3];
    float radius;
    float force;
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

  // Queue an explosion for the next recordFrame (game mechanic, not an edit
  // mode).
  void queueExplosion(const BlastCmd& blast)
  {
    m_blast = blast;
    m_blastPending = true;
  }

  // Debug: the cursor pixel, highlighted by the render to show what it hits.
  void setDebugMouse(float x, float y)
  {
    m_dbgMouseX = x;
    m_dbgMouseY = y;
  }

  // Debug: toggle the brick-grid + body-box wireframe overlays (P key).
  void setDebugWire(bool on) { m_debugWire = on; }

private:
  void buildGenerate();
  void buildWater();
  void buildRecount();
  void buildEdit();
  void buildBlast();
  void buildDebris();
  void buildRender();
  void buildTimestamps();
  void buildFaces();
  void buildAnchor();
  void buildRefine();
  void buildLabel();
  void buildBodyRegister();
  void buildBodyReduce();
  void buildBodyCollide();
  void buildBodyStamp();
  void buildBodyShed();
  void buildBodyStep();
  void buildBodyXform();
  void buildBodyPlace();
  void buildBodyPlaceStorage();
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
  // Explosions: the CPU-written input ray/params, and the GPU-published blast
  // (crater center from the raymarch + radius/force/active) that the impulse
  // and (later) debris passes read.
  WGPUBuffer m_blastInBuf = nullptr;
  WGPUBuffer m_blastBuf = nullptr;
  WGPUComputePipeline m_blastPipe = nullptr;
  WGPUBindGroup m_blastBg[2] = {nullptr,
                                nullptr}; // [srcIdx]: voxCur = current src
  WGPUComputePipeline m_blastImpulsePipe = nullptr;
  WGPUBindGroup m_blastImpulseBg = nullptr;
  BlastCmd m_blast{};
  bool m_blastPending = false;
  // Ballistic debris: a ring-allocated particle pool (pos/vel/voxel/life) the
  // blast ejects into and the advect pass flies + settles back into the grid as
  // powder.
  static constexpr uint32_t kDebrisMax =
      16384;                            // == DEBRIS_MAX in voxelCommon.wgsl
  WGPUBuffer m_debrisBuf = nullptr;     // kDebrisMax * 32 bytes
  WGPUBuffer m_debrisHeadBuf = nullptr; // atomic ring head
  WGPUComputePipeline m_debrisAdvectPipe = nullptr;
  WGPUBindGroup m_debrisAdvectBg[2] = {nullptr,
                                       nullptr}; // [srcIdx]: voxCur = src
  // Debris are drawn as instanced camera-facing quads, depth-tested against the
  // raymarch via a shared depth buffer (recreated on resize).
  WGPURenderPipeline m_debrisRenderPipe = nullptr;
  WGPUBindGroup m_debrisRenderBg = nullptr;
  WGPUTexture m_depthTex = nullptr;
  WGPUTextureView m_depthView = nullptr;
  int m_depthW = 0;
  int m_depthH = 0;
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
  uint64_t m_bodyBytes = 0; // one slot's window grid (label/rootSlot sizing)
  // Sparse brickmap body storage. Each body is a 12^3 grid of brick pointers
  // (m_bodyBrickGrid, slot s at s*BODYBRICKS); a non-empty pointer indexes an
  // 8^3 (512-voxel) brick in the shared m_brickPool. Bricks are allocated
  // lazily (only the filled ones) from m_brickFreeBuf, so a body costs ~its
  // real voxel count.
  static constexpr uint32_t kBrickPoolBricks =
      16384;                               // shared 8^3-brick capacity
  WGPUBuffer m_bodyBrickGrid = nullptr;    // kMaxBodies * BODYBRICKS pointers
  WGPUBuffer m_brickPool = nullptr;        // kBrickPoolBricks * 512 voxels
  WGPUBuffer m_brickFreeBuf = nullptr;     // [0]=count, [1..]=free brick ids
  WGPUBuffer m_bodyXformBuf = nullptr;     // array<Body, kMaxBodies>
  WGPUBuffer m_slotMetaBuf = nullptr;      // per-slot AABB/count/CoM/footprint
  WGPUBuffer m_slotMetaReadback = nullptr; // mapped to place the bodies
  WGPUBuffer m_rootSlotBuf = nullptr;      // per-brick component-root -> slot
  WGPUBuffer m_slotCountBuf = nullptr;     // atomic split-root ordering counter
  WGPUBuffer m_freeSlotBuf = nullptr; // [0]=free count, [1..]=free slot ids
  WGPUBuffer m_slotOccupiedBuf =
      nullptr; // per-slot atomic occupancy (GPU owns
               // allocation; register passes claim it)
  WGPUComputePipeline m_bodyRegisterPipe = nullptr;
  WGPUComputePipeline m_bodyReducePipe = nullptr;
  WGPUBindGroup m_bodyRegisterBg = nullptr;
  WGPUBindGroup m_bodyReduceBg = nullptr;
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
      nullptr; // culls sub-threshold world-fell slots
  WGPUBindGroup m_bodyPlaceStorageBg = nullptr;
  // GPU reap: frees baked slots (bricks + occupancy + flag) each frame,
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
                                  nullptr}; // [0] = one slot's label grid
  WGPUBuffer m_bodyLabelSlotBuf = nullptr;  // uniform: slot to label
  WGPUComputePipeline m_bodyLocalCcPipe = nullptr; // within-brick CC of a body
  WGPUBindGroup m_bodyLocalCcBg = nullptr;
  // Node-resolution label flood for a body split (mirrors the world-fell one).
  WGPUComputePipeline m_bodyNodeFloodPipe = nullptr;
  WGPUBindGroup m_bodyNodeFloodBg = nullptr;
  WGPUComputePipeline m_bodyScatterPipe = nullptr;
  WGPUBindGroup m_bodyScatterBg = nullptr;
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
  WGPUComputePipeline m_winMarkPipe = nullptr;
  WGPUBindGroup m_winBg[2] = {nullptr, nullptr};
  // Per window-brick world-solid count (0 == empty, skipped by the node flood).
  // 12^3 window bricks.
  static constexpr int kWinBricks = 12 * 12 * 12;
  WGPUBuffer m_winBrickOcc = nullptr;
  // Mixed-brick node CC: per-voxel within-brick component + per-(brick,
  // component) reached/min-label flag, so canopies (mixed bricks) converge fast
  // and a cut keeps its two sides separate.
  WGPUBuffer m_winVoxLocal = nullptr;
  WGPUBuffer m_winNodeReached = nullptr;
  // Node-resolution CC: per-window-brick "has any detached voxel" flag, so the
  // node flood dispatches one workgroup per brick and skips empty space.
  WGPUBuffer m_winDetBrick = nullptr;
  WGPUComputePipeline m_winClassifyPipe = nullptr;
  WGPUComputePipeline m_winLocalCcPipe = nullptr;
  // Node-resolution reachability flood (conducts "reached" over the node
  // graph).
  WGPUComputePipeline m_winNodeReachFloodPipe = nullptr;
  // Voxel-exact world fell stage 2/3: CC + extract over the window's detached
  // set (reuses m_windowBuf as the label buffer, m_rootSlot/m_slotMeta and the
  // sparse brickmap for body voxels).
  WGPUComputePipeline m_winDetachedCcPipe =
      nullptr; // within-brick CC of detached
  WGPUBindGroup m_winDetachedCcBg = nullptr;
  // Node-resolution label flood: floods min over the (brick, component) node
  // graph (adjacency discovered by brick-face scan), then scatters node labels
  // back to voxels.
  WGPUComputePipeline m_winNodeFloodPipe = nullptr;
  WGPUBindGroup m_winNodeFloodBg = nullptr;
  WGPUComputePipeline m_winScatterPipe = nullptr;
  WGPUBindGroup m_winScatterBg = nullptr;
  WGPUComputePipeline m_winRegisterPipe = nullptr;
  WGPUComputePipeline m_winReducePipe = nullptr;
  WGPUComputePipeline m_winExtractPipe = nullptr;
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
  bool m_debugWire = true; // wireframe overlays on by default
  int m_carvedSlot = -1;   // body slot the last carve hit, else -1
  // Sticky GPU flag (set by the carve-hit readback, ~1 frame stale): the last
  // carve hit world (vs a body or a miss). The reflood + world fell run the
  // same frame as the carve while this is set, so a sever extracts immediately;
  // held over empty space (no recent world hit) it clears and the machinery is
  // skipped.
  bool m_worldCarved = false;
  bool m_carveMapBusy = false;
  bool m_carvePendingResolve = false;
  struct CarveHitRb
  {
    WGPUBuffer buffer;
    int* slot;
    bool* worldCarved;
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
