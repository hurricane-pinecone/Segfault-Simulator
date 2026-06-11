#include "engine/webgpu/voxel/gpuVoxelWorld.h"

#include "engine/webgpu/wgpuUtil.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <engine/generated/embeddedWgsl.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sfs
{
namespace
{
using wgpu::makeBuffer;
using wgpu::makeShader;
using wgpu::storageEntry;
using wgpu::sv;

// Prepend the shared WGSL prelude to a stage's source.
std::string withCommon(std::string_view stage)
{
  return std::string(shaders::voxelCommonWgsl) + std::string(stage);
}

WGPUBindGroupLayout makeBgl(WGPUDevice device,
                            const WGPUBindGroupLayoutEntry* entries,
                            uint32_t count)
{
  WGPUBindGroupLayoutDescriptor d = {};
  d.entryCount = count;
  d.entries = entries;
  return wgpuDeviceCreateBindGroupLayout(device, &d);
}

WGPUPipelineLayout makePipelineLayout(WGPUDevice device,
                                      WGPUBindGroupLayout bgl)
{
  WGPUPipelineLayoutDescriptor d = {};
  d.bindGroupLayoutCount = 1;
  d.bindGroupLayouts = &bgl;
  return wgpuDeviceCreatePipelineLayout(device, &d);
}

WGPUComputePipeline makeComputePipeline(WGPUDevice device,
                                        WGPUShaderModule module,
                                        const char* entry,
                                        WGPUPipelineLayout layout)
{
  WGPUComputePipelineDescriptor d = {};
  d.layout = layout;
  d.compute.module = module;
  d.compute.entryPoint = sv(entry);
  return wgpuDeviceCreateComputePipeline(device, &d);
}

WGPUBindGroupEntry bufEntry(uint32_t binding, WGPUBuffer buffer, uint64_t size)
{
  WGPUBindGroupEntry e = {};
  e.binding = binding;
  e.buffer = buffer;
  e.size = size;
  return e;
}

// Reads back 4 GPU timestamps (water begin/end, render begin/end, in ns) and
// converts the deltas to milliseconds.
void onTsMapped(WGPUMapAsyncStatus status, WGPUStringView, void* ud1, void*)
{
  struct Rb
  {
    WGPUBuffer buffer;
    double* simMs;
    double* renderMs;
    double* totalMs;
    bool* busy;
  };
  auto* r = static_cast<Rb*>(ud1);
  if (status == WGPUMapAsyncStatus_Success)
  {
    const auto* ts = static_cast<const uint64_t*>(
        wgpuBufferGetConstMappedRange(r->buffer, 0, 32));
    if (ts)
    {
      *r->simMs = static_cast<double>(ts[1] - ts[0]) / 1.0e6;
      *r->renderMs = static_cast<double>(ts[3] - ts[2]) / 1.0e6;
      *r->totalMs = static_cast<double>(ts[3] - ts[0]) / 1.0e6;
    }
    wgpuBufferUnmap(r->buffer);
  }
  *r->busy = false;
}

// Mirrors the GPU motion state back to the CPU (one frame late) for gating
// only: active (collide pass + body-split target) and resting (stamp/free). The
// GPU owns placement and allocation, so this no longer drives the slot race.
void onBodyStateMapped(WGPUMapAsyncStatus status,
                       WGPUStringView,
                       void* ud1,
                       void*)
{
  struct Rb
  {
    WGPUBuffer buffer;
    GpuVoxelWorld::RigidBody* bodies;
    bool* busy;
  };
  auto* r = static_cast<Rb*>(ud1);
  if (status == WGPUMapAsyncStatus_Success)
  {
    const auto* m = static_cast<const float*>(wgpuBufferGetConstMappedRange(
        r->buffer, 0, 128u * GpuVoxelWorld::kMaxBodies));
    if (m)
      for (int s = 0; s < GpuVoxelWorld::kMaxBodies; ++s)
      {
        uint32_t flags = 0;
        std::memcpy(&flags, &m[s * 32], 4); // 32 floats/slot; flags first
        GpuVoxelWorld::RigidBody& b = r->bodies[s];
        b.active = (flags & 1u) != 0u;
        b.resting = (flags & 2u) != 0u; // bit1 = bake-ready
      }
    wgpuBufferUnmap(r->buffer);
  }
  *r->busy = false;
}

// Reads which body slot the last carve hit (-1 if it hit the world / nothing).
void onCarveHitMapped(WGPUMapAsyncStatus status,
                      WGPUStringView,
                      void* ud1,
                      void*)
{
  struct Rb
  {
    WGPUBuffer buffer;
    int* slot;
    bool* busy;
  };
  auto* r = static_cast<Rb*>(ud1);
  if (status == WGPUMapAsyncStatus_Success)
  {
    const auto* h = static_cast<const uint32_t*>(
        wgpuBufferGetConstMappedRange(r->buffer, 0, 8));
    if (h)
      *r->slot = (h[0] == 1u) ? static_cast<int>(h[1]) : -1;
    wgpuBufferUnmap(r->buffer);
  }
  *r->busy = false;
}
} // namespace

GpuVoxelWorld::GpuVoxelWorld(WebGpuContext& ctx)
    : m_ctx(ctx), m_device(ctx.device()), m_queue(ctx.queue())
{
  const uint64_t brickCount = static_cast<uint64_t>(kBG) * kBG * kBG;
  m_voxBytes = brickCount * 512 * sizeof(uint32_t);
  m_brickBytes = brickCount * sizeof(Brick);

  m_voxBuf[0] = makeBuffer(
      m_device, m_voxBytes, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
  m_voxBuf[1] = makeBuffer(
      m_device, m_voxBytes, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
  m_brickBuf = makeBuffer(m_device, m_brickBytes, WGPUBufferUsage_Storage);

  // Material palette (must match the ids in voxelCommon.wgsl). Colour is RGBA;
  // density / rigidity / emission are foundations for buoyancy, break-off, and
  // lighting (not all wired up yet). 256 entries, the rest zeroed.
  struct GpuMaterial
  {
    float color[4];
    float density;
    float rigidity;
    float emission;
    uint32_t flags;
  };
  static_assert(sizeof(GpuMaterial) == 32, "Material must match WGSL layout");
  GpuMaterial mats[256] = {};
  const auto setMat =
      [&](int id, float r, float g, float b, float a, float dens, float rigid)
  {
    mats[id] = {{r / 255.f, g / 255.f, b / 255.f, a}, dens, rigid, 0.0f, 0u};
  };
  setMat(1, 206, 192, 142, 1.0f, 1.5f, 0.2f); // sand
  setMat(2, 86, 168, 80, 1.0f, 1.0f, 0.5f);   // grass
  setMat(3, 122, 92, 60, 1.0f, 1.0f, 0.5f);   // dirt
  setMat(4, 108, 110, 124, 1.0f, 2.5f, 1.0f); // stone
  setMat(
      5, 101, 67, 33, 1.0f, 1.5f, 1.0f); // trunk (rigid -> resists crumbling)
  setMat(
      6,
      54,
      110,
      48,
      1.0f,
      0.08f,
      0.8f); // leaves: density very low (~8x the voxel count of the trunk, so
             // it must be low for the trunk to win the CoM); rigidity HIGH so
             // few leaves crumble -- a full canopy of leaf-rubble is too big
  setMat(7, 50, 110, 210, 0.6f, 1.0f, 0.0f); // water (translucent)
  setMat(8, 70, 72, 80, 0.5f, 0.2f, 0.0f);   // smoke (gas, light -> rises)
  setMat(9, 96, 88, 78, 1.0f, 1.5f, 0.0f);   // rubble (powder, falls + piles)
  m_materialBuf = makeBuffer(m_device,
                             sizeof(mats),
                             WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
  wgpuQueueWriteBuffer(m_queue, m_materialBuf, 0, mats, sizeof(mats));
  m_camBuf = makeBuffer(
      m_device, 64, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  m_frameBuf = makeBuffer(
      m_device, 16, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  m_editBuf = makeBuffer(
      m_device, 32, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);

  m_faceBytes = brickCount * 12 * sizeof(uint32_t);
  m_anchorBytes = brickCount * sizeof(uint32_t);
  m_faceBuf = makeBuffer(m_device, m_faceBytes, WGPUBufferUsage_Storage);
  m_anchorBuf[0] = makeBuffer(m_device, m_anchorBytes, WGPUBufferUsage_Storage);
  m_anchorBuf[1] = makeBuffer(m_device, m_anchorBytes, WGPUBufferUsage_Storage);
  m_floodCtlBuf = makeBuffer(m_device,
                             16,
                             WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst |
                                 WGPUBufferUsage_CopySrc);
  m_dirtyBuf = makeBuffer(m_device, m_anchorBytes, WGPUBufferUsage_Storage);
  m_labelBuf[0] = makeBuffer(m_device, m_anchorBytes, WGPUBufferUsage_Storage);
  m_labelBuf[1] = makeBuffer(m_device, m_anchorBytes, WGPUBufferUsage_Storage);

  m_bodyBytes =
      static_cast<uint64_t>(kBodyDim) * kBodyDim * kBodyDim * sizeof(uint32_t);
  m_bodyBuf = makeBuffer(m_device,
                         kBodyPoolBytes,
                         WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);

  // Size-class body storage (Stage B/C2). m_bodyDescBuf holds each slot's
  // (offset, dim) into the body voxel pool; m_classFreeBuf is the storage-block
  // free-list ([0]=count, [1..count]=block ids). C2a uses a single 96^3 class,
  // so there are kMaxBodies blocks; the descriptor defaults to the identity
  // layout.
  m_bodyDescBuf =
      makeBuffer(m_device,
                 static_cast<uint64_t>(kMaxBodies) * 2u * sizeof(uint32_t),
                 WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
  m_classFreeBuf =
      makeBuffer(m_device,
                 kClassFreeU32 * sizeof(uint32_t),
                 WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
  m_freeReqBuf =
      makeBuffer(m_device,
                 static_cast<uint64_t>(kMaxBodies + 1) * sizeof(uint32_t),
                 WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
  {
    std::vector<uint32_t> freeInit(kClassFreeU32);
    freeInit[0] = kMaxBodies; // all blocks free
    for (int i = 0; i < kMaxBodies; ++i)
      freeInit[i + 1] = static_cast<uint32_t>(i);
    wgpuQueueWriteBuffer(m_queue,
                         m_classFreeBuf,
                         0,
                         freeInit.data(),
                         freeInit.size() * sizeof(uint32_t));
    const uint32_t bodyVox =
        static_cast<uint32_t>(kBodyDim) * kBodyDim * kBodyDim;
    std::vector<uint32_t> descInit(static_cast<size_t>(kMaxBodies) * 2u);
    for (int s = 0; s < kMaxBodies; ++s)
    {
      descInit[s * 2 + 0] = static_cast<uint32_t>(s) * bodyVox;
      descInit[s * 2 + 1] = kBodyDim;
    }
    wgpuQueueWriteBuffer(m_queue,
                         m_bodyDescBuf,
                         0,
                         descInit.data(),
                         descInit.size() * sizeof(uint32_t));
  }
  m_bodyXformBuf =
      makeBuffer(m_device, 96 * kMaxBodies, WGPUBufferUsage_Storage);
  // Rigid-body state: 8 vec4 (128 bytes) per slot.
  m_bodyStateBuf =
      makeBuffer(m_device,
                 128 * kMaxBodies,
                 WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst |
                     WGPUBufferUsage_CopySrc);
  m_bodyStateReadback =
      makeBuffer(m_device,
                 128 * kMaxBodies,
                 WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead);
  m_bodyStepUBuf = makeBuffer(
      m_device, 16, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  m_placeUBuf = makeBuffer(
      m_device, 16, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  m_bodyStateRb =
      BodyStateRb{m_bodyStateReadback, m_bodies, &m_bodyStateMapBusy};
  // The step pass reads every slot each frame, so the state must start cleared
  // (all-inactive) rather than as uninitialized GPU memory.
  {
    const uint8_t zeros[128 * kMaxBodies] = {};
    wgpuQueueWriteBuffer(m_queue, m_bodyStateBuf, 0, zeros, sizeof(zeros));
  }
  m_slotMetaBuf = makeBuffer(m_device,
                             64 * kMaxBodies,
                             WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst |
                                 WGPUBufferUsage_CopySrc);
  m_slotMetaReadback =
      makeBuffer(m_device,
                 64 * kMaxBodies,
                 WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead);
  // Indexed by body-grid-local index (BODYDIM^3) by the window/body CC, which
  // exceeds the brick count once BODYDIM > kBG, so size it to a full body grid.
  m_rootSlotBuf = makeBuffer(m_device, m_bodyBytes, WGPUBufferUsage_Storage);
  m_slotCountBuf = makeBuffer(
      m_device, 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
  m_freeSlotBuf = makeBuffer(m_device,
                             4 * (kMaxBodies + 1),
                             WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
  m_slotOccupiedBuf =
      makeBuffer(m_device,
                 4 * kMaxBodies,
                 WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
  {
    const uint32_t zeros[kMaxBodies] = {};
    wgpuQueueWriteBuffer(m_queue, m_slotOccupiedBuf, 0, zeros, 4u * kMaxBodies);
  }
  m_slotRb = SlotMetaRb{m_slotMetaReadback, m_bodies, &m_bodyMapBusy};
  // Per-slot contact record (stride 8 ints): count, sumXYZ, max penetration.
  m_collideBuf = makeBuffer(m_device,
                            32 * kMaxBodies,
                            WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst |
                                WGPUBufferUsage_CopySrc);
  m_collideReadback =
      makeBuffer(m_device,
                 4 * kMaxBodies,
                 WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead);
  m_collideRb = CollideRb{m_collideReadback, m_bodies, &m_collideMapBusy};
  m_carveHitBuf = makeBuffer(m_device,
                             32,
                             WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst |
                                 WGPUBufferUsage_CopySrc);
  m_carveHitReadback = makeBuffer(
      m_device, 8, WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead);
  m_carveRb = CarveHitRb{m_carveHitReadback, &m_carvedSlot, &m_carveMapBusy};
  m_bodyLabelBuf[0] =
      makeBuffer(m_device, m_bodyBytes, WGPUBufferUsage_Storage);
  m_bodyLabelBuf[1] =
      makeBuffer(m_device, m_bodyBytes, WGPUBufferUsage_Storage);
  m_bodyLabelSlotBuf = makeBuffer(
      m_device, 16, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  m_dbgMouseBuf = makeBuffer(m_device,
                             16,
                             WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst |
                                 WGPUBufferUsage_Storage); // .z written by prep
  m_bodyArgsBuf =
      makeBuffer(m_device,
                 16,
                 WGPUBufferUsage_Storage | WGPUBufferUsage_Indirect |
                     WGPUBufferUsage_CopyDst);
  m_windowBuf[0] = makeBuffer(m_device, m_bodyBytes, WGPUBufferUsage_Storage);
  m_windowBuf[1] = makeBuffer(m_device, m_bodyBytes, WGPUBufferUsage_Storage);

  buildTimestamps();
  buildGenerate();
  buildWater();
  buildRecount();
  buildEdit();
  buildRender();
  buildFaces();
  buildAnchor();
  buildRefine();
  buildLabel();
  buildBodyRegister();
  buildBodyReduce();
  buildBodyExtract();
  buildBodyCollide();
  buildBodyStamp();
  buildBodyShed();
  buildBodyStep();
  buildBodyXform();
  buildBodyPlace();
  buildBodyPlaceStorage();
  buildBodyFreeStorage();
  buildBodyReap();
  buildBodyPrep();
  buildBodyLabel();
  buildBodySplit();
  buildWindowAnchor();
  buildWindowSplit();
}

GpuVoxelWorld::~GpuVoxelWorld()
{
  for (WGPUBindGroup bg : {m_genBg,
                           m_waterBg[0],
                           m_waterBg[1],
                           m_recBg[0],
                           m_recBg[1],
                           m_editBg[0],
                           m_editBg[1],
                           m_renderBg[0],
                           m_renderBg[1],
                           m_facesBg,
                           m_seedBg,
                           m_floodBg[0],
                           m_floodBg[1],
                           m_refineBg,
                           m_labelInitBg,
                           m_labelFloodBg[0],
                           m_labelFloodBg[1],
                           m_bodyRegisterBg,
                           m_bodyReduceBg,
                           m_bodyExtractBg,
                           m_bodyCollideBg,
                           m_bodyStampBg,
                           m_bodyLabelInitBg,
                           m_bodyLabelFloodBg[0],
                           m_bodyLabelFloodBg[1],
                           m_bodyRecolorBg,
                           m_bodySplitBg})
    if (bg)
      wgpuBindGroupRelease(bg);

  if (m_genPipe)
    wgpuComputePipelineRelease(m_genPipe);
  if (m_waterPipe)
    wgpuComputePipelineRelease(m_waterPipe);
  if (m_recPipe)
    wgpuComputePipelineRelease(m_recPipe);
  if (m_editPipe)
    wgpuComputePipelineRelease(m_editPipe);
  if (m_renderPipe)
    wgpuRenderPipelineRelease(m_renderPipe);
  if (m_facesPipe)
    wgpuComputePipelineRelease(m_facesPipe);
  if (m_seedPipe)
    wgpuComputePipelineRelease(m_seedPipe);
  if (m_floodPipe)
    wgpuComputePipelineRelease(m_floodPipe);
  if (m_refinePipe)
    wgpuComputePipelineRelease(m_refinePipe);
  if (m_labelInitPipe)
    wgpuComputePipelineRelease(m_labelInitPipe);
  if (m_labelFloodPipe)
    wgpuComputePipelineRelease(m_labelFloodPipe);
  if (m_bodyRegisterPipe)
    wgpuComputePipelineRelease(m_bodyRegisterPipe);
  if (m_bodyReducePipe)
    wgpuComputePipelineRelease(m_bodyReducePipe);
  if (m_bodyExtractPipe)
    wgpuComputePipelineRelease(m_bodyExtractPipe);
  if (m_bodyCollidePipe)
    wgpuComputePipelineRelease(m_bodyCollidePipe);
  if (m_bodyStampPipe)
    wgpuComputePipelineRelease(m_bodyStampPipe);
  if (m_bodyShedPipe)
    wgpuComputePipelineRelease(m_bodyShedPipe);
  if (m_bodyStepPipe)
    wgpuComputePipelineRelease(m_bodyStepPipe);
  if (m_bodyXformPipe)
    wgpuComputePipelineRelease(m_bodyXformPipe);
  if (m_bodyPlacePipe)
    wgpuComputePipelineRelease(m_bodyPlacePipe);
  if (m_bodyPlaceStoragePipe)
    wgpuComputePipelineRelease(m_bodyPlaceStoragePipe);
  if (m_bodyFreeStoragePipe)
    wgpuComputePipelineRelease(m_bodyFreeStoragePipe);
  if (m_bodyReapPipe)
    wgpuComputePipelineRelease(m_bodyReapPipe);
  if (m_bodyLabelInitPipe)
    wgpuComputePipelineRelease(m_bodyLabelInitPipe);
  if (m_bodyLabelFloodPipe)
    wgpuComputePipelineRelease(m_bodyLabelFloodPipe);
  if (m_bodyRecolorPipe)
    wgpuComputePipelineRelease(m_bodyRecolorPipe);
  for (WGPUComputePipeline p : {m_bodySplitRegisterPipe,
                                m_bodySplitReducePipe,
                                m_bodySplitFootprintPipe,
                                m_bodySplitExtractPipe,
                                m_bodySplitClearParentPipe,
                                m_winInitPipe,
                                m_winFloodPipe,
                                m_winMarkPipe,
                                m_winLabelInitPipe,
                                m_winLabelFloodPipe,
                                m_winRegisterPipe,
                                m_winReducePipe,
                                m_winExtractPipe})
    if (p)
      wgpuComputePipelineRelease(p);

  for (WGPUBuffer b :
       {m_voxBuf[0],        m_voxBuf[1],       m_brickBuf,
        m_materialBuf,      m_camBuf,          m_frameBuf,
        m_editBuf,          m_faceBuf,         m_anchorBuf[0],
        m_anchorBuf[1],     m_floodCtlBuf,     m_dirtyBuf,
        m_labelBuf[0],      m_labelBuf[1],     m_bodyBuf,
        m_bodyDescBuf,      m_classFreeBuf,    m_freeReqBuf,
        m_bodyXformBuf,     m_slotMetaBuf,     m_slotMetaReadback,
        m_rootSlotBuf,      m_slotCountBuf,    m_freeSlotBuf,
        m_collideBuf,       m_collideReadback, m_carveHitBuf,
        m_carveHitReadback, m_bodyLabelBuf[0], m_bodyLabelBuf[1],
        m_bodyLabelSlotBuf, m_dbgMouseBuf,     m_windowBuf[0],
        m_windowBuf[1],     m_bodyStateBuf,    m_bodyStateReadback,
        m_bodyStepUBuf,     m_placeUBuf,       m_slotOccupiedBuf,
        m_tsResolve,        m_tsReadback})
    if (b)
      wgpuBufferRelease(b);
  if (m_tsQuery)
    wgpuQuerySetRelease(m_tsQuery);
}

void GpuVoxelWorld::buildTimestamps()
{
  m_hasTs = m_ctx.hasTimestamps();
  if (!m_hasTs)
    return;
  WGPUQuerySetDescriptor qd = {};
  qd.type = WGPUQueryType_Timestamp;
  qd.count = 4;
  m_tsQuery = wgpuDeviceCreateQuerySet(m_device, &qd);
  m_tsResolve = makeBuffer(
      m_device, 32, WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc);
  m_tsReadback = makeBuffer(
      m_device, 32, WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead);
  m_tsRb = TsReadback{
      m_tsReadback, &m_gpuSimMs, &m_gpuRenderMs, &m_gpuTotalMs, &m_tsMapBusy};
}

void GpuVoxelWorld::buildGenerate()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelGenerateWgsl).c_str());
  WGPUBindGroupLayoutEntry e[3] = {
      storageEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 3);
  m_genPipe = makeComputePipeline(
      m_device, module, "generate", makePipelineLayout(m_device, bgl));

  WGPUBindGroupEntry be[3] = {bufEntry(0, m_voxBuf[0], m_voxBytes),
                              bufEntry(1, m_voxBuf[1], m_voxBytes),
                              bufEntry(2, m_brickBuf, m_brickBytes)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 3;
  d.entries = be;
  m_genBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildWater()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelWaterWgsl).c_str());
  WGPUBindGroupLayoutEntry e[4] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 4);
  m_waterPipe = makeComputePipeline(
      m_device, module, "water", makePipelineLayout(m_device, bgl));

  for (int i = 0; i < 2; ++i)
  {
    WGPUBindGroupEntry be[4] = {bufEntry(0, m_voxBuf[i], m_voxBytes),
                                bufEntry(1, m_voxBuf[1 - i], m_voxBytes),
                                bufEntry(2, m_brickBuf, m_brickBytes),
                                bufEntry(3, m_frameBuf, 16)};
    WGPUBindGroupDescriptor d = {};
    d.layout = bgl;
    d.entryCount = 4;
    d.entries = be;
    m_waterBg[i] = wgpuDeviceCreateBindGroup(m_device, &d);
  }
}

void GpuVoxelWorld::buildRecount()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelRecountWgsl).c_str());
  WGPUBindGroupLayoutEntry e[3] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 3);
  m_recPipe = makeComputePipeline(
      m_device, module, "recount", makePipelineLayout(m_device, bgl));

  for (int i = 0; i < 2; ++i)
  {
    WGPUBindGroupEntry be[3] = {bufEntry(0, m_voxBuf[i], m_voxBytes),
                                bufEntry(1, m_voxBuf[1 - i], m_voxBytes),
                                bufEntry(2, m_brickBuf, m_brickBytes)};
    WGPUBindGroupDescriptor d = {};
    d.layout = bgl;
    d.entryCount = 3;
    d.entries = be;
    m_recBg[i] = wgpuDeviceCreateBindGroup(m_device, &d);
  }
}

void GpuVoxelWorld::buildEdit()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelEditWgsl).c_str());
  WGPUBindGroupLayoutEntry e[8] = {
      storageEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform),
      storageEntry(4, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(5, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          6, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(7, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 8);
  m_editPipe = makeComputePipeline(
      m_device, module, "edit", makePipelineLayout(m_device, bgl));

  for (int i = 0; i < 2; ++i)
  {
    WGPUBindGroupEntry be[8] = {bufEntry(0, m_voxBuf[i], m_voxBytes),
                                bufEntry(1, m_voxBuf[1 - i], m_voxBytes),
                                bufEntry(2, m_brickBuf, m_brickBytes),
                                bufEntry(3, m_editBuf, 32),
                                bufEntry(4, m_dirtyBuf, m_anchorBytes),
                                bufEntry(5, m_bodyBuf, kBodyPoolBytes),
                                bufEntry(6, m_bodyXformBuf, 96 * kMaxBodies),
                                bufEntry(7, m_carveHitBuf, 32)};
    WGPUBindGroupDescriptor d = {};
    d.layout = bgl;
    d.entryCount = 8;
    d.entries = be;
    m_editBg[i] = wgpuDeviceCreateBindGroup(m_device, &d);
  }
}

void GpuVoxelWorld::buildRender()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelRenderWgsl).c_str());
  WGPUBindGroupLayoutEntry e[10] = {
      storageEntry(0, WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform),
      storageEntry(
          1, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          2, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          3, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          4, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          5, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          6, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(7, WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform),
      storageEntry(
          8, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          9, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 10);
  WGPUPipelineLayout layout = makePipelineLayout(m_device, bgl);

  WGPUColorTargetState colorTarget = {};
  colorTarget.format = m_ctx.format();
  colorTarget.writeMask = WGPUColorWriteMask_All;
  WGPUFragmentState frag = {};
  frag.module = module;
  frag.entryPoint = sv("fs_main");
  frag.targetCount = 1;
  frag.targets = &colorTarget;
  WGPURenderPipelineDescriptor d = {};
  d.layout = layout;
  d.vertex.module = module;
  d.vertex.entryPoint = sv("vs_main");
  d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  d.primitive.frontFace = WGPUFrontFace_CCW;
  d.primitive.cullMode = WGPUCullMode_None;
  d.multisample.count = 1;
  d.multisample.mask = 0xFFFFFFFFu;
  d.fragment = &frag;
  m_renderPipe = wgpuDeviceCreateRenderPipeline(m_device, &d);

  for (int i = 0; i < 2; ++i)
  {
    WGPUBindGroupEntry be[10] = {bufEntry(0, m_camBuf, 64),
                                 bufEntry(1, m_voxBuf[i], m_voxBytes),
                                 bufEntry(2, m_brickBuf, m_brickBytes),
                                 bufEntry(3, m_anchorBuf[0], m_anchorBytes),
                                 bufEntry(4, m_bodyBuf, kBodyPoolBytes),
                                 bufEntry(5, m_bodyXformBuf, 96 * kMaxBodies),
                                 bufEntry(6, m_labelBuf[0], m_anchorBytes),
                                 bufEntry(7, m_dbgMouseBuf, 16),
                                 bufEntry(8, m_materialBuf, 32 * 256),
                                 bufEntry(9, m_bodyArgsBuf, 16)};
    WGPUBindGroupDescriptor bd = {};
    bd.layout = bgl;
    bd.entryCount = 10;
    bd.entries = be;
    m_renderBg[i] = wgpuDeviceCreateBindGroup(m_device, &bd);
  }
}

void GpuVoxelWorld::buildFaces()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelFacesWgsl).c_str());
  WGPUBindGroupLayoutEntry e[2] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 2);
  m_facesPipe = makeComputePipeline(
      m_device, module, "faces", makePipelineLayout(m_device, bgl));

  WGPUBindGroupEntry be[2] = {bufEntry(0, m_voxBuf[0], m_voxBytes),
                              bufEntry(1, m_faceBuf, m_faceBytes)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 2;
  d.entries = be;
  m_facesBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildAnchor()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelAnchorWgsl).c_str());
  // One shared layout for both entry points (seed ignores the bindings it does
  // not use): faceBuf, bricks, anchorIn (read), anchorOut, floodCtl (write).
  WGPUBindGroupLayoutEntry e[5] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          1, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(4, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 5);
  WGPUPipelineLayout layout = makePipelineLayout(m_device, bgl);
  m_seedPipe = makeComputePipeline(m_device, module, "seed", layout);
  m_floodPipe = makeComputePipeline(m_device, module, "flood", layout);

  // Seed writes the authoritative anchorBuf[0]; the other bindings are present
  // for layout compatibility but unread by seed.
  WGPUBindGroupEntry se[5] = {bufEntry(0, m_faceBuf, m_faceBytes),
                              bufEntry(1, m_brickBuf, m_brickBytes),
                              bufEntry(2, m_anchorBuf[1], m_anchorBytes),
                              bufEntry(3, m_anchorBuf[0], m_anchorBytes),
                              bufEntry(4, m_floodCtlBuf, 16)};
  WGPUBindGroupDescriptor sd = {};
  sd.layout = bgl;
  sd.entryCount = 5;
  sd.entries = se;
  m_seedBg = wgpuDeviceCreateBindGroup(m_device, &sd);

  for (int i = 0; i < 2; ++i)
  {
    // Round parity i: read anchorBuf[i], write anchorBuf[1-i].
    WGPUBindGroupEntry fe[5] = {bufEntry(0, m_faceBuf, m_faceBytes),
                                bufEntry(1, m_brickBuf, m_brickBytes),
                                bufEntry(2, m_anchorBuf[i], m_anchorBytes),
                                bufEntry(3, m_anchorBuf[1 - i], m_anchorBytes),
                                bufEntry(4, m_floodCtlBuf, 16)};
    WGPUBindGroupDescriptor fd = {};
    fd.layout = bgl;
    fd.entryCount = 5;
    fd.entries = fe;
    m_floodBg[i] = wgpuDeviceCreateBindGroup(m_device, &fd);
  }
}

void GpuVoxelWorld::buildRefine()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelRefineWgsl).c_str());
  WGPUBindGroupLayoutEntry e[4] = {
      storageEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 4);
  m_refinePipe = makeComputePipeline(
      m_device, module, "refine", makePipelineLayout(m_device, bgl));

  WGPUBindGroupEntry be[4] = {bufEntry(0, m_voxBuf[0], m_voxBytes),
                              bufEntry(1, m_voxBuf[1], m_voxBytes),
                              bufEntry(2, m_anchorBuf[0], m_anchorBytes),
                              bufEntry(3, m_dirtyBuf, m_anchorBytes)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 4;
  d.entries = be;
  m_refineBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildLabel()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelLabelWgsl).c_str());
  WGPUBindGroupLayoutEntry e[4] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          1, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 4);
  WGPUPipelineLayout layout = makePipelineLayout(m_device, bgl);
  m_labelInitPipe = makeComputePipeline(m_device, module, "labelInit", layout);
  m_labelFloodPipe =
      makeComputePipeline(m_device, module, "labelFlood", layout);

  // labelInit seeds labelBuf[0]; binding 2 (labelIn) is present for layout
  // only.
  WGPUBindGroupEntry ie[4] = {bufEntry(0, m_faceBuf, m_faceBytes),
                              bufEntry(1, m_anchorBuf[0], m_anchorBytes),
                              bufEntry(2, m_labelBuf[1], m_anchorBytes),
                              bufEntry(3, m_labelBuf[0], m_anchorBytes)};
  WGPUBindGroupDescriptor id = {};
  id.layout = bgl;
  id.entryCount = 4;
  id.entries = ie;
  m_labelInitBg = wgpuDeviceCreateBindGroup(m_device, &id);

  for (int i = 0; i < 2; ++i)
  {
    // Round parity i: read labelBuf[i], write labelBuf[1-i].
    WGPUBindGroupEntry fe[4] = {bufEntry(0, m_faceBuf, m_faceBytes),
                                bufEntry(1, m_anchorBuf[0], m_anchorBytes),
                                bufEntry(2, m_labelBuf[i], m_anchorBytes),
                                bufEntry(3, m_labelBuf[1 - i], m_anchorBytes)};
    WGPUBindGroupDescriptor fd = {};
    fd.layout = bgl;
    fd.entryCount = 4;
    fd.entries = fe;
    m_labelFloodBg[i] = wgpuDeviceCreateBindGroup(m_device, &fd);
  }
}

void GpuVoxelWorld::buildBodyRegister()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelBodyRegisterWgsl).c_str());
  WGPUBindGroupLayoutEntry e[4] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          3, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 4);
  m_bodyRegisterPipe = makeComputePipeline(
      m_device, module, "registerRoots", makePipelineLayout(m_device, bgl));

  WGPUBindGroupEntry be[4] = {bufEntry(0, m_labelBuf[0], m_anchorBytes),
                              bufEntry(1, m_rootSlotBuf, m_bodyBytes),
                              bufEntry(2, m_slotCountBuf, 4),
                              bufEntry(3, m_freeSlotBuf, 4 * (kMaxBodies + 1))};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 4;
  d.entries = be;
  m_bodyRegisterBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildBodyReduce()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelBodyReduceWgsl).c_str());
  WGPUBindGroupLayoutEntry e[5] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          1, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          3, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(4, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 5);
  m_bodyReducePipe = makeComputePipeline(
      m_device, module, "reduce", makePipelineLayout(m_device, bgl));

  WGPUBindGroupEntry be[5] = {bufEntry(0, m_anchorBuf[0], m_anchorBytes),
                              bufEntry(1, m_faceBuf, m_faceBytes),
                              bufEntry(2, m_labelBuf[0], m_anchorBytes),
                              bufEntry(3, m_rootSlotBuf, m_bodyBytes),
                              bufEntry(4, m_slotMetaBuf, 64 * kMaxBodies)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 5;
  d.entries = be;
  m_bodyReduceBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildBodyExtract()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelBodyExtractWgsl).c_str());
  WGPUBindGroupLayoutEntry e[7] = {
      storageEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(4, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          5, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          6, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 7);
  m_bodyExtractPipe = makeComputePipeline(
      m_device, module, "extract", makePipelineLayout(m_device, bgl));

  WGPUBindGroupEntry be[7] = {bufEntry(0, m_voxBuf[0], m_voxBytes),
                              bufEntry(1, m_voxBuf[1], m_voxBytes),
                              bufEntry(2, m_anchorBuf[0], m_anchorBytes),
                              bufEntry(3, m_slotMetaBuf, 64 * kMaxBodies),
                              bufEntry(4, m_bodyBuf, kBodyPoolBytes),
                              bufEntry(5, m_labelBuf[0], m_anchorBytes),
                              bufEntry(6, m_rootSlotBuf, m_bodyBytes)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 7;
  d.entries = be;
  m_bodyExtractBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildBodyCollide()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelBodyCollideWgsl).c_str());
  WGPUBindGroupLayoutEntry e[5] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          1, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          4, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 5);
  m_bodyCollidePipe = makeComputePipeline(
      m_device, module, "collide", makePipelineLayout(m_device, bgl));

  WGPUBindGroupEntry be[5] = {bufEntry(0, m_bodyBuf, kBodyPoolBytes),
                              bufEntry(1, m_voxBuf[0], m_voxBytes),
                              bufEntry(2, m_bodyXformBuf, 96 * kMaxBodies),
                              bufEntry(3, m_collideBuf, 32 * kMaxBodies),
                              bufEntry(4, m_brickBuf, m_brickBytes)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 5;
  d.entries = be;
  m_bodyCollideBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildBodyStamp()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelBodyStampWgsl).c_str());
  WGPUBindGroupLayoutEntry e[4] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          3, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 4);
  m_bodyStampPipe = makeComputePipeline(
      m_device, module, "stamp", makePipelineLayout(m_device, bgl));

  WGPUBindGroupEntry be[4] = {bufEntry(0, m_bodyBuf, kBodyPoolBytes),
                              bufEntry(1, m_voxBuf[0], m_voxBytes),
                              bufEntry(2, m_voxBuf[1], m_voxBytes),
                              bufEntry(3, m_bodyXformBuf, 96 * kMaxBodies)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 4;
  d.entries = be;
  m_bodyStampBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildBodyShed()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelBodyShedWgsl).c_str());
  WGPUBindGroupLayoutEntry e[6] = {
      storageEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          3, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          4, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(5, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 6);
  m_bodyShedPipe = makeComputePipeline(
      m_device, module, "shed", makePipelineLayout(m_device, bgl));
  for (int i = 0; i < 2; ++i)
  {
    WGPUBindGroupEntry be[6] = {bufEntry(0, m_bodyBuf, kBodyPoolBytes),
                                bufEntry(1, m_voxBuf[i], m_voxBytes),
                                bufEntry(2, m_bodyXformBuf, 96 * kMaxBodies),
                                bufEntry(3, m_bodyStateBuf, 128 * kMaxBodies),
                                bufEntry(4, m_materialBuf, 32 * 256),
                                bufEntry(5, m_frameBuf, 16)};
    WGPUBindGroupDescriptor d = {};
    d.layout = bgl;
    d.entryCount = 6;
    d.entries = be;
    m_bodyShedBg[i] = wgpuDeviceCreateBindGroup(m_device, &d);
  }
}

void GpuVoxelWorld::buildBodyStep()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelBodyStepWgsl).c_str());
  WGPUBindGroupLayoutEntry e[3] = {
      storageEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          1, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 3);
  m_bodyStepPipe = makeComputePipeline(
      m_device, module, "stepBodies", makePipelineLayout(m_device, bgl));
  WGPUBindGroupEntry be[3] = {bufEntry(0, m_bodyStateBuf, 128 * kMaxBodies),
                              bufEntry(1, m_collideBuf, 32 * kMaxBodies),
                              bufEntry(2, m_bodyStepUBuf, 16)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 3;
  d.entries = be;
  m_bodyStepBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildBodyXform()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelBodyXformWgsl).c_str());
  WGPUBindGroupLayoutEntry e[3] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 3);
  m_bodyXformPipe = makeComputePipeline(
      m_device, module, "buildXform", makePipelineLayout(m_device, bgl));
  WGPUBindGroupEntry be[3] = {bufEntry(0, m_bodyStateBuf, 128 * kMaxBodies),
                              bufEntry(1, m_bodyXformBuf, 96 * kMaxBodies),
                              bufEntry(2, m_bodyDescBuf, 8 * kMaxBodies)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 3;
  d.entries = be;
  m_bodyXformBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildBodyPlaceStorage()
{
  WGPUShaderModule module = makeShader(
      m_device, withCommon(shaders::voxelBodyPlaceStorageWgsl).c_str());
  WGPUBindGroupLayoutEntry e[5] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform),
      storageEntry(4, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 5);
  m_bodyPlaceStoragePipe = makeComputePipeline(
      m_device, module, "placeStorage", makePipelineLayout(m_device, bgl));
  WGPUBindGroupEntry be[5] = {bufEntry(0, m_slotMetaBuf, 64 * kMaxBodies),
                              bufEntry(1, m_bodyDescBuf, 8 * kMaxBodies),
                              bufEntry(2, m_classFreeBuf, 4 * kClassFreeU32),
                              bufEntry(3, m_placeUBuf, 16),
                              bufEntry(4, m_slotOccupiedBuf, 4 * kMaxBodies)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 5;
  d.entries = be;
  m_bodyPlaceStorageBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildBodyFreeStorage()
{
  WGPUShaderModule module = makeShader(
      m_device, withCommon(shaders::voxelBodyFreeStorageWgsl).c_str());
  WGPUBindGroupLayoutEntry e[3] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 3);
  m_bodyFreeStoragePipe = makeComputePipeline(
      m_device, module, "freeStorage", makePipelineLayout(m_device, bgl));
  WGPUBindGroupEntry be[3] = {bufEntry(0, m_bodyDescBuf, 8 * kMaxBodies),
                              bufEntry(1, m_classFreeBuf, 4 * kClassFreeU32),
                              bufEntry(2, m_freeReqBuf, 4 * (kMaxBodies + 1))};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 3;
  d.entries = be;
  m_bodyFreeStorageBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildBodyReap()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelBodyReapWgsl).c_str());
  WGPUBindGroupLayoutEntry e[4] = {
      storageEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          1, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 4);
  m_bodyReapPipe = makeComputePipeline(
      m_device, module, "reap", makePipelineLayout(m_device, bgl));
  WGPUBindGroupEntry be[4] = {bufEntry(0, m_bodyStateBuf, 128 * kMaxBodies),
                              bufEntry(1, m_bodyDescBuf, 8 * kMaxBodies),
                              bufEntry(2, m_classFreeBuf, 4 * kClassFreeU32),
                              bufEntry(3, m_slotOccupiedBuf, 4 * kMaxBodies)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 4;
  d.entries = be;
  m_bodyReapBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildBodyPrep()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelBodyPrepWgsl).c_str());
  WGPUBindGroupLayoutEntry e[3] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 3);
  m_bodyPrepPipe = makeComputePipeline(
      m_device, module, "prep", makePipelineLayout(m_device, bgl));
  WGPUBindGroupEntry be[3] = {bufEntry(0, m_bodyStateBuf, 128 * kMaxBodies),
                              bufEntry(1, m_bodyArgsBuf, 16),
                              bufEntry(2, m_dbgMouseBuf, 16)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 3;
  d.entries = be;
  m_bodyPrepBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildBodyPlace()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelBodyPlaceWgsl).c_str());
  WGPUBindGroupLayoutEntry e[3] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 3);
  m_bodyPlacePipe = makeComputePipeline(
      m_device, module, "placeBodies", makePipelineLayout(m_device, bgl));
  WGPUBindGroupEntry be[3] = {bufEntry(0, m_slotMetaBuf, 64 * kMaxBodies),
                              bufEntry(1, m_bodyStateBuf, 128 * kMaxBodies),
                              bufEntry(2, m_placeUBuf, 16)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 3;
  d.entries = be;
  m_bodyPlaceBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildBodyLabel()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelBodyLabelWgsl).c_str());
  WGPUBindGroupLayoutEntry e[5] = {
      storageEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          1, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform),
      storageEntry(
          4, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 5);
  WGPUPipelineLayout layout = makePipelineLayout(m_device, bgl);
  m_bodyLabelInitPipe =
      makeComputePipeline(m_device, module, "labelInit", layout);
  m_bodyLabelFloodPipe =
      makeComputePipeline(m_device, module, "labelFlood", layout);
  m_bodyRecolorPipe = makeComputePipeline(m_device, module, "recolor", layout);

  const uint64_t pool = kBodyPoolBytes;
  // init seeds labelBuf[0]; binding 1 present for layout only.
  WGPUBindGroupEntry ie[5] = {bufEntry(0, m_bodyBuf, pool),
                              bufEntry(1, m_bodyLabelBuf[1], m_bodyBytes),
                              bufEntry(2, m_bodyLabelBuf[0], m_bodyBytes),
                              bufEntry(3, m_bodyLabelSlotBuf, 16),
                              bufEntry(4, m_bodyDescBuf, 8 * kMaxBodies)};
  WGPUBindGroupDescriptor id = {};
  id.layout = bgl;
  id.entryCount = 5;
  id.entries = ie;
  m_bodyLabelInitBg = wgpuDeviceCreateBindGroup(m_device, &id);

  for (int i = 0; i < 2; ++i)
  {
    WGPUBindGroupEntry fe[5] = {bufEntry(0, m_bodyBuf, pool),
                                bufEntry(1, m_bodyLabelBuf[i], m_bodyBytes),
                                bufEntry(2, m_bodyLabelBuf[1 - i], m_bodyBytes),
                                bufEntry(3, m_bodyLabelSlotBuf, 16),
                                bufEntry(4, m_bodyDescBuf, 8 * kMaxBodies)};
    WGPUBindGroupDescriptor fd = {};
    fd.layout = bgl;
    fd.entryCount = 5;
    fd.entries = fe;
    m_bodyLabelFloodBg[i] = wgpuDeviceCreateBindGroup(m_device, &fd);
  }

  WGPUBindGroupEntry re[5] = {bufEntry(0, m_bodyBuf, pool),
                              bufEntry(1, m_bodyLabelBuf[0], m_bodyBytes),
                              bufEntry(2, m_bodyLabelBuf[1], m_bodyBytes),
                              bufEntry(3, m_bodyLabelSlotBuf, 16),
                              bufEntry(4, m_bodyDescBuf, 8 * kMaxBodies)};
  WGPUBindGroupDescriptor rd = {};
  rd.layout = bgl;
  rd.entryCount = 5;
  rd.entries = re;
  m_bodyRecolorBg = wgpuDeviceCreateBindGroup(m_device, &rd);
}

void GpuVoxelWorld::buildBodySplit()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelBodySplitWgsl).c_str());
  WGPUBindGroupLayoutEntry e[9] = {
      storageEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          1, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(4, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(5, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(6, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform),
      storageEntry(
          7, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          8, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 9);
  WGPUPipelineLayout layout = makePipelineLayout(m_device, bgl);
  m_bodySplitRegisterPipe =
      makeComputePipeline(m_device, module, "registerRoots", layout);
  m_bodySplitReducePipe =
      makeComputePipeline(m_device, module, "reduce", layout);
  m_bodySplitFootprintPipe =
      makeComputePipeline(m_device, module, "footprint", layout);
  m_bodySplitExtractPipe =
      makeComputePipeline(m_device, module, "extract", layout);
  m_bodySplitClearParentPipe =
      makeComputePipeline(m_device, module, "clearParent", layout);

  WGPUBindGroupEntry be[9] = {bufEntry(0, m_bodyBuf, kBodyPoolBytes),
                              bufEntry(1, m_bodyLabelBuf[0], m_bodyBytes),
                              bufEntry(2, m_rootSlotBuf, m_bodyBytes),
                              bufEntry(3, m_slotMetaBuf, 64 * kMaxBodies),
                              bufEntry(4, m_slotOccupiedBuf, 4 * kMaxBodies),
                              bufEntry(5, m_slotCountBuf, 4),
                              bufEntry(6, m_bodyLabelSlotBuf, 16),
                              bufEntry(7, m_materialBuf, 32 * 256),
                              bufEntry(8, m_bodyDescBuf, 8 * kMaxBodies)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 9;
  d.entries = be;
  m_bodySplitBg = wgpuDeviceCreateBindGroup(m_device, &d);
}

void GpuVoxelWorld::buildWindowAnchor()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelWindowAnchorWgsl).c_str());
  WGPUBindGroupLayoutEntry e[7] = {
      storageEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          3, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          4, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          5, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(6, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 7);
  WGPUPipelineLayout layout = makePipelineLayout(m_device, bgl);
  m_winInitPipe = makeComputePipeline(m_device, module, "winInit", layout);
  m_winFloodPipe = makeComputePipeline(m_device, module, "winFlood", layout);
  m_winMarkPipe = makeComputePipeline(m_device, module, "winMark", layout);

  for (int i = 0; i < 2; ++i)
  {
    WGPUBindGroupEntry be[7] = {bufEntry(0, m_voxBuf[0], m_voxBytes),
                                bufEntry(1, m_voxBuf[1], m_voxBytes),
                                bufEntry(2, m_brickBuf, m_brickBytes),
                                bufEntry(3, m_anchorBuf[0], m_anchorBytes),
                                bufEntry(4, m_carveHitBuf, 32),
                                bufEntry(5, m_windowBuf[i], m_bodyBytes),
                                bufEntry(6, m_windowBuf[1 - i], m_bodyBytes)};
    WGPUBindGroupDescriptor d = {};
    d.layout = bgl;
    d.entryCount = 7;
    d.entries = be;
    m_winBg[i] = wgpuDeviceCreateBindGroup(m_device, &d);
  }
}

void GpuVoxelWorld::buildWindowSplit()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelWindowSplitWgsl).c_str());
  const auto RW = WGPUBufferBindingType_Storage;
  const auto RO = WGPUBufferBindingType_ReadOnlyStorage;
  const auto S = WGPUShaderStage_Compute;
  const auto pass = [&](std::initializer_list<WGPUBindGroupLayoutEntry> le,
                        const char* entry)
      -> std::pair<WGPUComputePipeline, WGPUBindGroupLayout>
  {
    std::vector<WGPUBindGroupLayoutEntry> v(le);
    WGPUBindGroupLayout bgl =
        makeBgl(m_device, v.data(), static_cast<uint32_t>(v.size()));
    return {makeComputePipeline(
                m_device, module, entry, makePipelineLayout(m_device, bgl)),
            bgl};
  };
  const auto bg =
      [&](WGPUBindGroupLayout bgl, std::initializer_list<WGPUBindGroupEntry> be)
  {
    std::vector<WGPUBindGroupEntry> v(be);
    WGPUBindGroupDescriptor d = {};
    d.layout = bgl;
    d.entryCount = static_cast<uint32_t>(v.size());
    d.entries = v.data();
    return wgpuDeviceCreateBindGroup(m_device, &d);
  };
  const uint64_t labelBytes =
      m_bodyBytes; // DIM^3 u32, reusing the window buffers
  const uint64_t metaBytes = 64u * kMaxBodies;

  auto li = pass({storageEntry(0, S, RW),
                  storageEntry(2, S, RO),
                  storageEntry(3, S, RO),
                  storageEntry(5, S, RW)},
                 "labelInit");
  m_winLabelInitPipe = li.first;
  m_winLabelInitBg = bg(li.second,
                        {bufEntry(0, m_voxBuf[0], m_voxBytes),
                         bufEntry(2, m_brickBuf, m_brickBytes),
                         bufEntry(3, m_carveHitBuf, 32),
                         bufEntry(5, m_windowBuf[0], labelBytes)});

  auto fl =
      pass({storageEntry(4, S, RO), storageEntry(5, S, RW)}, "labelFlood");
  m_winLabelFloodPipe = fl.first;
  for (int i = 0; i < 2; ++i)
    m_winLabelFloodBg[i] = bg(fl.second,
                              {bufEntry(4, m_windowBuf[i], labelBytes),
                               bufEntry(5, m_windowBuf[1 - i], labelBytes)});

  auto rg = pass(
      {storageEntry(4, S, RO), storageEntry(6, S, RW), storageEntry(8, S, RW)},
      "registerRoots");
  m_winRegisterPipe = rg.first;
  m_winRegisterBg = bg(rg.second,
                       {bufEntry(4, m_windowBuf[0], labelBytes),
                        bufEntry(6, m_rootSlotBuf, m_bodyBytes),
                        bufEntry(8, m_slotOccupiedBuf, 4 * kMaxBodies)});

  auto rd = pass({storageEntry(0, S, RW), // vox0 (read material to skip leaves)
                  storageEntry(2, S, RO), // bricks (for wIdx)
                  storageEntry(3, S, RO),
                  storageEntry(4, S, RO),
                  storageEntry(6, S, RW),
                  storageEntry(7, S, RW)},
                 "reduce");
  m_winReducePipe = rd.first;
  m_winReduceBg = bg(rd.second,
                     {bufEntry(0, m_voxBuf[0], m_voxBytes),
                      bufEntry(2, m_brickBuf, m_brickBytes),
                      bufEntry(3, m_carveHitBuf, 32),
                      bufEntry(4, m_windowBuf[0], labelBytes),
                      bufEntry(6, m_rootSlotBuf, m_bodyBytes),
                      bufEntry(7, m_slotMetaBuf, metaBytes)});

  auto ex = pass({storageEntry(0, S, RW),
                  storageEntry(1, S, RW),
                  storageEntry(2, S, RO),
                  storageEntry(3, S, RO),
                  storageEntry(4, S, RO),
                  storageEntry(6, S, RW),
                  storageEntry(7, S, RW),
                  storageEntry(9, S, RO),
                  storageEntry(10, S, RW),
                  storageEntry(11, S, RO)},
                 "extract");
  m_winExtractPipe = ex.first;
  // Two variants so vox0 is always the current src: sub-threshold scrap is
  // written as falling powder, which must land in src (the buffer next frame's
  // water tick reads) -- writing both buffers would duplicate it.
  for (int i = 0; i < 2; ++i)
    m_winExtractBg[i] = bg(ex.second,
                           {bufEntry(0, m_voxBuf[i], m_voxBytes),
                            bufEntry(1, m_voxBuf[1 - i], m_voxBytes),
                            bufEntry(2, m_brickBuf, m_brickBytes),
                            bufEntry(3, m_carveHitBuf, 32),
                            bufEntry(4, m_windowBuf[0], labelBytes),
                            bufEntry(6, m_rootSlotBuf, m_bodyBytes),
                            bufEntry(7, m_slotMetaBuf, metaBytes),
                            bufEntry(9, m_materialBuf, 32 * 256),
                            bufEntry(10, m_bodyBuf, kBodyPoolBytes),
                            bufEntry(11, m_bodyDescBuf, 8 * kMaxBodies)});
}

void GpuVoxelWorld::generate()
{
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(m_device, nullptr);

  const auto dispatch = [&](WGPUComputePipeline pipe,
                            WGPUBindGroup bg,
                            uint32_t x,
                            uint32_t y,
                            uint32_t z)
  {
    WGPUComputePassEncoder cp =
        wgpuCommandEncoderBeginComputePass(enc, nullptr);
    wgpuComputePassEncoderSetPipeline(cp, pipe);
    wgpuComputePassEncoderSetBindGroup(cp, 0, bg, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(cp, x, y, z);
    wgpuComputePassEncoderEnd(cp);
    wgpuComputePassEncoderRelease(cp);
  };

  dispatch(m_genPipe, m_genBg, kBG, kBG, kBG);

  // Establish the connectivity baseline: face masks, seed the ground bricks,
  // then flood ground-anchoring to convergence. Terrain is fully
  // ground-connected by construction, so afterwards every occupied brick is
  // anchored (the value settles in anchorBuf[0] after an even round count).
  dispatch(m_facesPipe, m_facesBg, kBG, kBG, kBG);
  const uint32_t brickGroups = (kBG * kBG * kBG + 63) / 64;
  dispatch(m_seedPipe, m_seedBg, brickGroups, 1, 1);
  for (int round = 0; round < 64; ++round)
    dispatch(m_floodPipe, m_floodBg[round & 1], brickGroups, 1, 1);
  // No detached set yet, but seed labelBuf[0] to the sentinel so the render
  // reads a defined value before the first carve.
  dispatch(m_labelInitPipe, m_labelInitBg, brickGroups, 1, 1);

  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(m_queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);
}

void GpuVoxelWorld::stepBody(double dt)
{
  // Integration moved to the GPU (voxelBodyStep); the CPU just forwards dt to
  // the step pass run in recordFrame.
  m_stepDt = static_cast<float>(dt);
}

void GpuVoxelWorld::recordFrame(WGPUCommandEncoder enc,
                                WGPUTextureView view,
                                const float* cam16,
                                const EditCmd* edit,
                                uint32_t frame)
{
  // Read back the previous frame's resolved timestamps while idle (one tick
  // late, so a copy never targets the mapped buffer).
  if (m_hasTs && m_tsPendingResolve && !m_tsMapBusy)
  {
    m_tsMapBusy = true;
    m_tsPendingResolve = false;
    WGPUBufferMapCallbackInfo mci = {};
    mci.mode = WGPUCallbackMode_AllowProcessEvents;
    mci.callback = onTsMapped;
    mci.userdata1 = &m_tsRb;
    wgpuBufferMapAsync(m_tsReadback, WGPUMapMode_Read, 0, 32, mci);
  }

  // Mirror the GPU motion state back (one frame late) for the CPU's gating
  // only: active (collide pass + which slots are free) and resting
  // (stamp/free). The GPU now owns placement and allocation, so this never
  // feeds the slot race.
  if (m_bodyStatePendingResolve && !m_bodyStateMapBusy)
  {
    m_bodyStateMapBusy = true;
    m_bodyStatePendingResolve = false;
    WGPUBufferMapCallbackInfo mci = {};
    mci.mode = WGPUCallbackMode_AllowProcessEvents;
    mci.callback = onBodyStateMapped;
    mci.userdata1 = &m_bodyStateRb;
    wgpuBufferMapAsync(
        m_bodyStateReadback, WGPUMapMode_Read, 0, 128u * kMaxBodies, mci);
  }

  // Read back which body slot the last carve hit (one frame late).
  if (m_carvePendingResolve && !m_carveMapBusy)
  {
    m_carveMapBusy = true;
    m_carvePendingResolve = false;
    WGPUBufferMapCallbackInfo mci = {};
    mci.mode = WGPUCallbackMode_AllowProcessEvents;
    mci.callback = onCarveHitMapped;
    mci.userdata1 = &m_carveRb;
    wgpuBufferMapAsync(m_carveHitReadback, WGPUMapMode_Read, 0, 8, mci);
  }

  wgpuQueueWriteBuffer(m_queue, m_camBuf, 0, cam16, 64);
  const uint32_t fdata[4] = {frame, 0, 0, 0};
  wgpuQueueWriteBuffer(m_queue, m_frameBuf, 0, fdata, sizeof(fdata));
  const float dbgMouse[4] = {m_dbgMouseX, m_dbgMouseY, 0.0f, 0.0f};
  wgpuQueueWriteBuffer(m_queue, m_dbgMouseBuf, 0, dbgMouse, 16);

  // GPU body motion: integrate (gravity/topple/land) then build the per-slot
  // transform matrices, before any pass that reads them (edit/collide/stamp/
  // render). The step consumes last frame's collide flags + the dt uniform.
  {
    const float stepU[4] = {m_stepDt, 0.0f, 0.0f, 0.0f};
    wgpuQueueWriteBuffer(m_queue, m_bodyStepUBuf, 0, stepU, 16);
    const auto motionPass = [&](WGPUComputePipeline pipe, WGPUBindGroup bg)
    {
      WGPUComputePassEncoder p =
          wgpuCommandEncoderBeginComputePass(enc, nullptr);
      wgpuComputePassEncoderSetPipeline(p, pipe);
      wgpuComputePassEncoderSetBindGroup(p, 0, bg, 0, nullptr);
      wgpuComputePassEncoderDispatchWorkgroups(
          p, (kMaxBodies + 63) / 64, 1, 1); // one thread per slot (wg size 64)
      wgpuComputePassEncoderEnd(p);
      wgpuComputePassEncoderRelease(p);
    };
    motionPass(m_bodyStepPipe, m_bodyStepBg);
    motionPass(m_bodyXformPipe, m_bodyXformBg);
    // Mirror the stepped state back to the CPU one frame late.
    if (!m_bodyStateMapBusy)
    {
      wgpuCommandEncoderCopyBufferToBuffer(
          enc, m_bodyStateBuf, 0, m_bodyStateReadback, 0, 128u * kMaxBodies);
      m_bodyStatePendingResolve = true;
    }
  }

  if (edit)
  {
    const float ep[8] = {edit->origin[0],
                         edit->origin[1],
                         edit->origin[2],
                         edit->radius,
                         edit->dir[0],
                         edit->dir[1],
                         edit->dir[2],
                         static_cast<float>(edit->mode)};
    wgpuQueueWriteBuffer(m_queue, m_editBuf, 0, ep, sizeof(ep));

    WGPUComputePassEncoder ce =
        wgpuCommandEncoderBeginComputePass(enc, nullptr);
    wgpuComputePassEncoderSetPipeline(ce, m_editPipe);
    wgpuComputePassEncoderSetBindGroup(ce, 0, m_editBg[m_srcIdx], 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(ce, 1, 1, 1);
    wgpuComputePassEncoderEnd(ce);
    wgpuComputePassEncoderRelease(ce);

    // Read back which body the carve hit (one frame late) to drive the split.
    if (!m_carveMapBusy)
    {
      wgpuCommandEncoderCopyBufferToBuffer(
          enc, m_carveHitBuf, 0, m_carveHitReadback, 0, 8);
      m_carvePendingResolve = true;
    }
  }

  const bool doTs = m_hasTs && !m_tsMapBusy && !m_tsPendingResolve;

  // Water tick: move src -> dst, then swap. dst's dynamic cells were wiped by
  // last frame's cleanup; its static terrain persists (never cleared/copied).
  WGPUComputePassTimestampWrites simTsw = {m_tsQuery, 0, 1};
  WGPUComputePassDescriptor cpd = {};
  if (doTs)
    cpd.timestampWrites = &simTsw;
  WGPUComputePassEncoder cp = wgpuCommandEncoderBeginComputePass(enc, &cpd);
  wgpuComputePassEncoderSetPipeline(cp, m_waterPipe);
  wgpuComputePassEncoderSetBindGroup(cp, 0, m_waterBg[m_srcIdx], 0, nullptr);
  wgpuComputePassEncoderDispatchWorkgroups(cp, kBG, kBG, kBG);
  wgpuComputePassEncoderEnd(cp);
  wgpuComputePassEncoderRelease(cp);
  m_srcIdx = 1 - m_srcIdx;

  // Recompute brick occupancy on the new state so brick-skip stays correct as
  // water moves between bricks.
  WGPUComputePassEncoder cr = wgpuCommandEncoderBeginComputePass(enc, nullptr);
  wgpuComputePassEncoderSetPipeline(cr, m_recPipe);
  wgpuComputePassEncoderSetBindGroup(cr, 0, m_recBg[m_srcIdx], 0, nullptr);
  wgpuComputePassEncoderDispatchWorkgroups(cr, kBG, kBG, kBG);
  wgpuComputePassEncoderEnd(cr);
  wgpuComputePassEncoderRelease(cr);

  // On a carve, static solidity changed -> re-derive ground anchoring. A carve
  // removes support, which the monotone flood can't undo in place, so reset
  // (rebuild faces from the post-carve voxels, reseed ground) and reflood to
  // convergence. A severed piece never re-reaches ground -> its bricks stay
  // unanchored, which the render tints. Brick-resolution, so a full reflood is
  // cheap; only runs on carve frames.
  if (edit && edit->mode == 1)
  {
    const auto detectPass = [&](WGPUComputePipeline pipe,
                                WGPUBindGroup bg,
                                uint32_t x,
                                uint32_t y,
                                uint32_t z)
    {
      WGPUComputePassEncoder p =
          wgpuCommandEncoderBeginComputePass(enc, nullptr);
      wgpuComputePassEncoderSetPipeline(p, pipe);
      wgpuComputePassEncoderSetBindGroup(p, 0, bg, 0, nullptr);
      wgpuComputePassEncoderDispatchWorkgroups(p, x, y, z);
      wgpuComputePassEncoderEnd(p);
      wgpuComputePassEncoderRelease(p);
    };
    // Brick-level ground anchoring (cut-unaware: raw solidity). winInit reads
    // this only as a boundary condition -- is the bulk beyond the window
    // grounded -- so the cut, which sits at the window centre far from the
    // boundary, needs no brick-level resolution here. The voxel-exact window
    // pass owns all detachment near the cut.
    detectPass(m_facesPipe, m_facesBg, kBG, kBG, kBG);
    const uint32_t brickGroups = (kBG * kBG * kBG + 63) / 64;
    detectPass(m_seedPipe, m_seedBg, brickGroups, 1, 1);
    for (int round = 0; round < 64; ++round)
      detectPass(m_floodPipe, m_floodBg[round & 1], brickGroups, 1, 1);
  }

  // Auto-fell: drop severed pieces the instant they are cut. Only a carve that
  // hit the WORLD requests a world fell; a body carve (m_carvedSlot >= 0) must
  // not, or the spuriously-fired world fell would hog the shared body-meta
  // readback and starve the body split that the same cut needs.
  if (edit && edit->mode == 1 && m_carvedSlot < 0)
    m_fellRequested = true;
  if (edit && edit->mode == 1)
    m_fellHold =
        4; // a carve may claim new body slots -> process full range briefly

  const auto bodyPass = [&](WGPUComputePipeline pipe,
                            WGPUBindGroup bg,
                            uint32_t x,
                            uint32_t y,
                            uint32_t z)
  {
    WGPUComputePassEncoder p = wgpuCommandEncoderBeginComputePass(enc, nullptr);
    wgpuComputePassEncoderSetPipeline(p, pipe);
    wgpuComputePassEncoderSetBindGroup(p, 0, bg, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(p, x, y, z);
    wgpuComputePassEncoderEnd(p);
    wgpuComputePassEncoderRelease(p);
  };
  // Per-frame body passes (collide/shed/stamp) size themselves from the GPU's
  // activeHigh via the prep pass's indirect args -- no CPU count.
  const auto bodyPassIndirect = [&](WGPUComputePipeline pipe, WGPUBindGroup bg)
  {
    WGPUComputePassEncoder p = wgpuCommandEncoderBeginComputePass(enc, nullptr);
    wgpuComputePassEncoderSetPipeline(p, pipe);
    wgpuComputePassEncoderSetBindGroup(p, 0, bg, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroupsIndirect(p, m_bodyArgsBuf, 0);
    wgpuComputePassEncoderEnd(p);
    wgpuComputePassEncoderRelease(p);
  };
  const uint32_t brickGroups = (kBG * kBG * kBG + 63) / 64;
  const uint32_t bodyZ = static_cast<uint32_t>(kBodyDim / 4 * kMaxBodies);
  const auto reanchorRelabel = [&]()
  {
    bodyPass(m_facesPipe, m_facesBg, kBG, kBG, kBG);
    bodyPass(m_seedPipe, m_seedBg, brickGroups, 1, 1);
    for (int round = 0; round < 64; ++round)
      bodyPass(m_floodPipe, m_floodBg[round & 1], brickGroups, 1, 1);
    bodyPass(m_labelInitPipe, m_labelInitBg, brickGroups, 1, 1);
    for (int round = 0; round < 128; ++round)
      bodyPass(m_labelFloodPipe, m_labelFloodBg[round & 1], brickGroups, 1, 1);
  };

  // anyResting gates only the cosmetic post-bake re-anchor now (stamp + freeing
  // are GPU-driven and run every frame). The per-frame body dispatch SIZES and
  // the render's loop bound are GPU-driven too (m_bodyPrepPipe -> indirect args
  // + dbgMouse.z), so the CPU never sizes body work from the 1-frame-stale
  // mirror
  // -- that staleness was the lag-vs-vanishing bug.
  bool anyResting = false;
  for (int s = 0; s < kMaxBodies; ++s)
    if (m_bodies[s].active && m_bodies[s].resting)
      anyResting = true;

  // Voxel-exact world fell: mark the detached set (bit 4) in a DIM^3 window
  // around the cut, label it into connected components, reduce each to a
  // world-coord AABB, and extract every one into a body slot the register pass
  // claimed atomically from the GPU occupancy bitmap (no CPU free-list, no
  // serialization -- a full pool just yields no claim). Marking is COUPLED with
  // extraction (same frame) so a marked voxel is cleared the frame it is
  // marked.
  if (m_fellRequested && m_carvedSlot < 0)
  {
    m_fellRequested = false;
    const uint32_t wg = kBodyDim / 4;
    // Window anchor: voxel-exact ground flood -> bit 4 on the detached set.
    bodyPass(m_winInitPipe, m_winBg[1], wg, wg, wg);
    for (int r = 0; r < 128; ++r)
      bodyPass(m_winFloodPipe, m_winBg[r & 1], wg, wg, wg);
    bodyPass(m_winMarkPipe, m_winBg[0], wg, wg, wg);

    // Per slot: aabbMin = +world (for atomicMin), everything else 0.
    int32_t metaInit[16 * kMaxBodies] = {};
    for (int s = 0; s < kMaxBodies; ++s)
    {
      metaInit[s * 16 + 0] = kWorld;
      metaInit[s * 16 + 1] = kWorld;
      metaInit[s * 16 + 2] = kWorld;
    }
    wgpuQueueWriteBuffer(m_queue, m_slotMetaBuf, 0, metaInit, sizeof(metaInit));
    bodyPass(m_winLabelInitPipe, m_winLabelInitBg, wg, wg, wg);
    for (int r = 0; r < 128; ++r)
      bodyPass(m_winLabelFloodPipe, m_winLabelFloodBg[r & 1], wg, wg, wg);
    bodyPass(m_winRegisterPipe, m_winRegisterBg, wg, wg, wg);
    bodyPass(m_winReducePipe, m_winReduceBg, wg, wg, wg);
    // Allocate a storage block per just-reduced slot, recording its (offset,
    // dim) in the descriptor that the extract writes to and the transform
    // publishes.
    bodyPass(m_bodyPlaceStoragePipe,
             m_bodyPlaceStorageBg,
             (kMaxBodies + 63) / 64,
             1,
             1);
    // Extract must cover the whole pool: the just-claimed slots are GPU-side
    // and not yet in the CPU mirror, so a bounded dispatch could miss a new
    // body and clear it from the world without filling its grid (the vanishing
    // top).
    bodyPass(m_winExtractPipe, m_winExtractBg[m_srcIdx], wg, wg, bodyZ);
    // GPU placement: reduced metadata -> motion state, in the same encoder.
    const uint32_t placeU[4] = {0u, 0u, 0u, 0u}; // isSplit = 0 (world fell)
    wgpuQueueWriteBuffer(m_queue, m_placeUBuf, 0, placeU, 16);
    bodyPass(m_bodyPlacePipe, m_bodyPlaceBg, (kMaxBodies + 63) / 64, 1, 1);
  }

  // Re-publish the transform AFTER the fell: the frame-start xform ran before
  // these bodies existed, so without this a freshly placed body renders +
  // collides from a stale slot (wrong block offset / pose) for one frame -- it
  // vanishes at the cut and "pops in" once the next frame's xform catches up.
  // Republishing here gives new bodies a correct transform the frame they are
  // born.
  bodyPass(m_bodyXformPipe, m_bodyXformBg, (kMaxBodies + 63) / 64, 1, 1);

  // GPU-driven sizing: scan the (now post-fell, up-to-date) state flags ->
  // activeHigh -> the per-frame body passes' indirect dispatch args + the
  // render's loop bound (dbgMouse.z). Must run after the fell's place so
  // freshly placed bodies are counted this frame -- that ordering is what stops
  // the "vanishing top" without any CPU readback.
  bodyPass(m_bodyPrepPipe, m_bodyPrepBg, 1, 1, 1);

  // Clear the collide flags every frame (in-encoder, after this frame's step
  // has already read the prior result -- a queue write would run before the
  // whole command buffer and wipe what the step still needs).
  wgpuCommandEncoderClearBuffer(enc, m_collideBuf, 0, 32u * kMaxBodies);
  bodyPassIndirect(m_bodyCollidePipe, m_bodyCollideBg);

  // Break-off: shed hard-impact body voxels (recorded by the step in state slot
  // 6) into the current src buffer as rubble. Runs into voxCur before the fluid
  // tick so the shed rubble falls + piles this frame.
  bodyPassIndirect(m_bodyShedPipe, m_bodyShedBg[m_srcIdx]);

  // Stamp every baked body (flags.z) back into the world, then reap it: the GPU
  // reap returns its storage block to the free-list, releases its occupancy
  // bit, and clears its state flag. Both run every frame (stamp indirect-sized,
  // reap over the pool), each a no-op for non-baked slots, so a body is stamped
  // and retired the frame it bakes -- no CPU free loop, no mirror-driven
  // freeing.
  bodyPassIndirect(m_bodyStampPipe, m_bodyStampBg);
  bodyPass(m_bodyReapPipe, m_bodyReapBg, (kMaxBodies + 63) / 64, 1, 1);

  // Cosmetic catch-up: recount brick occupancy + re-anchor so a freshly stamped
  // log reads as grounded (the render hash-tints unanchored solids). Gated on
  // the mirror's anyResting, whose state snapshot is captured before the reap
  // clears the flag, so it fires exactly the frame after a bake. This is a
  // debug-tint refresh, not control, so a frame of mirror lag is harmless.
  if (anyResting)
  {
    bodyPass(m_recPipe, m_recBg[m_srcIdx], kBG, kBG, kBG);
    reanchorRelabel();
  }

  // Body split: label the carved body's connected components and extract every
  // component but one into free slots (the parent keeps one). Mutually
  // exclusive with the world fell above (a carve hits one target).
  if (m_carvedSlot >= 0 && m_carvedSlot < kMaxBodies && edit &&
      edit->mode == 1 && m_bodies[m_carvedSlot].active &&
      !m_bodies[m_carvedSlot].landed)
  {
    const uint32_t parent = static_cast<uint32_t>(m_carvedSlot);
    const uint32_t slotData[4] = {parent, 0, 0, 0};
    wgpuQueueWriteBuffer(m_queue, m_bodyLabelSlotBuf, 0, slotData, 16);
    const uint32_t g = kBodyDim / 4;
    bodyPass(m_bodyLabelInitPipe, m_bodyLabelInitBg, g, g, g);
    for (int r = 0; r < 128; ++r)
      bodyPass(m_bodyLabelFloodPipe, m_bodyLabelFloodBg[r & 1], g, g, g);

    // slotCount is the root-ordering counter (k==0 -> parent, rest claim a
    // slot).
    const uint32_t zeroCount = 0u;
    wgpuQueueWriteBuffer(m_queue, m_slotCountBuf, 0, &zeroCount, 4);
    int32_t metaInit[16 * kMaxBodies] = {};
    for (int s = 0; s < kMaxBodies; ++s)
    {
      metaInit[s * 16 + 0] = kBodyDim;
      metaInit[s * 16 + 1] = kBodyDim;
      metaInit[s * 16 + 2] = kBodyDim;
    }
    wgpuQueueWriteBuffer(m_queue, m_slotMetaBuf, 0, metaInit, sizeof(metaInit));
    bodyPass(m_bodySplitRegisterPipe, m_bodySplitBg, g, g, g);
    bodyPass(m_bodySplitReducePipe, m_bodySplitBg, g, g, g);
    // Allocate storage blocks for the split's CHILDREN (placeU.y=parent is
    // skipped so the parent keeps its block). placeU is queue-written below but
    // applies before the whole command buffer, so this pass reads the split
    // values.
    const uint32_t placeU[4] = {1u, parent, 0u, 0u};
    wgpuQueueWriteBuffer(m_queue, m_placeUBuf, 0, placeU, 16);
    bodyPass(m_bodyPlaceStoragePipe,
             m_bodyPlaceStorageBg,
             (kMaxBodies + 63) / 64,
             1,
             1);
    bodyPass(m_bodySplitExtractPipe, m_bodySplitBg, g, g, g * kMaxBodies);
    bodyPass(m_bodySplitClearParentPipe, m_bodySplitBg, g, g, g);
    // GPU placement (split mode): the place pass reads the parent's LIVE state
    // for the children's in-place frame, so no CPU capture is needed.
    bodyPass(m_bodyPlacePipe, m_bodyPlaceBg, (kMaxBodies + 63) / 64, 1, 1);
    m_fellRequested = false; // a body carve makes no world detachment
  }

  WGPURenderPassColorAttachment color = {};
  color.view = view;
  color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  color.loadOp = WGPULoadOp_Clear;
  color.storeOp = WGPUStoreOp_Store;
  color.clearValue = WGPUColor{0.02, 0.02, 0.04, 1.0};
  WGPURenderPassTimestampWrites renderTsw = {m_tsQuery, 2, 3};
  WGPURenderPassDescriptor pass = {};
  pass.colorAttachmentCount = 1;
  pass.colorAttachments = &color;
  if (doTs)
    pass.timestampWrites = &renderTsw;
  WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc, &pass);
  wgpuRenderPassEncoderSetPipeline(rp, m_renderPipe);
  wgpuRenderPassEncoderSetBindGroup(rp, 0, m_renderBg[m_srcIdx], 0, nullptr);
  wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
  wgpuRenderPassEncoderEnd(rp);
  wgpuRenderPassEncoderRelease(rp);

  if (doTs)
  {
    wgpuCommandEncoderResolveQuerySet(enc, m_tsQuery, 0, 4, m_tsResolve, 0);
    wgpuCommandEncoderCopyBufferToBuffer(
        enc, m_tsResolve, 0, m_tsReadback, 0, 32);
    m_tsPendingResolve = true;
  }
}

} // namespace sfs
