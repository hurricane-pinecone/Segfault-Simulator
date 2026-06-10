#include "engine/webgpu/voxel/gpuVoxelWorld.h"

#include "engine/webgpu/wgpuUtil.h"

#include <cmath>

#include <engine/generated/embeddedWgsl.h>

#include <string>
#include <string_view>

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

// Reads the per-slot meta (AABB, counts, CoM-sum, footprint-sum) for the whole
// body pool, placing each extracted chunk: pivot about its contact footprint
// and a topple axis perpendicular to its CoM lever (so it tips toward its heavy
// side). One readback per fell event; slots with no component go inactive.
void onSlotMetaMapped(WGPUMapAsyncStatus status,
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
    const auto* m = static_cast<const int32_t*>(wgpuBufferGetConstMappedRange(
        r->buffer, 0, 64u * GpuVoxelWorld::kMaxBodies));
    if (m)
    {
      for (int s = 0; s < GpuVoxelWorld::kMaxBodies; ++s)
      {
        GpuVoxelWorld::RigidBody& b = r->bodies[s];
        if (b.active)
          continue; // a live body already falling -- leave it running
        const int base = s * 16;
        if (m[base + 6] <= 0 || m[base + 10] <= 0 || m[base + 13] <= 0)
          continue; // free slot with no component this fell
        const float footX =
            static_cast<float>(m[base + 11]) / static_cast<float>(m[base + 13]);
        const float footZ =
            static_cast<float>(m[base + 12]) / static_cast<float>(m[base + 13]);
        b.center[0] = static_cast<float>(m[base + 0]) + footX;
        b.center[1] = static_cast<float>(m[base + 1]);
        b.center[2] = static_cast<float>(m[base + 2]) + footZ;
        b.pivot[0] = footX;
        b.pivot[1] = 0.0f;
        b.pivot[2] = footZ;

        const float inv = 1.0f / static_cast<float>(m[base + 10]);
        const float leverX = static_cast<float>(m[base + 7]) * inv - footX;
        const float leverZ = static_cast<float>(m[base + 9]) * inv - footZ;
        const float len = std::sqrt(leverX * leverX + leverZ * leverZ);
        float dx = 1.0f;
        float dz = 0.0f;
        if (len > 0.5f) // balanced chunks get a default nudge direction
        {
          dx = leverX / len;
          dz = leverZ / len;
        }
        b.axis[0] = dz;
        b.axis[1] = 0.0f;
        b.axis[2] = -dx;
        b.active = true;
        b.wasActive = false; // stepBody seeds the sim on the first active frame
      }
    }
    wgpuBufferUnmap(r->buffer);
  }
  *r->busy = false;
}

// Reads each slot's body-vs-world collision flag (did that body land).
void onCollideMapped(WGPUMapAsyncStatus status,
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
    const auto* f = static_cast<const uint32_t*>(wgpuBufferGetConstMappedRange(
        r->buffer, 0, 4u * GpuVoxelWorld::kMaxBodies));
    if (f)
      for (int s = 0; s < GpuVoxelWorld::kMaxBodies; ++s)
        r->bodies[s].collided = (f[s] != 0u);
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
                         m_bodyBytes * kMaxBodies,
                         WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
  m_bodyXformBuf =
      makeBuffer(m_device,
                 96 * kMaxBodies,
                 WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
  m_slotMetaBuf = makeBuffer(m_device,
                             64 * kMaxBodies,
                             WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst |
                                 WGPUBufferUsage_CopySrc);
  m_slotMetaReadback =
      makeBuffer(m_device,
                 64 * kMaxBodies,
                 WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead);
  m_rootSlotBuf = makeBuffer(m_device, m_anchorBytes, WGPUBufferUsage_Storage);
  m_slotCountBuf = makeBuffer(
      m_device, 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
  m_freeSlotBuf = makeBuffer(m_device,
                             4 * (kMaxBodies + 1),
                             WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
  m_slotRb = SlotMetaRb{m_slotMetaReadback, m_bodies, &m_bodyMapBusy};
  m_collideBuf = makeBuffer(m_device,
                            4 * kMaxBodies,
                            WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst |
                                WGPUBufferUsage_CopySrc);
  m_collideReadback =
      makeBuffer(m_device,
                 4 * kMaxBodies,
                 WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead);
  m_collideRb = CollideRb{m_collideReadback, m_bodies, &m_collideMapBusy};

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
}

GpuVoxelWorld::~GpuVoxelWorld()
{
  for (WGPUBindGroup bg :
       {m_genBg,           m_waterBg[0],     m_waterBg[1],   m_recBg[0],
        m_recBg[1],        m_editBg[0],      m_editBg[1],    m_renderBg[0],
        m_renderBg[1],     m_facesBg,        m_seedBg,       m_floodBg[0],
        m_floodBg[1],      m_refineBg,       m_labelInitBg,  m_labelFloodBg[0],
        m_labelFloodBg[1], m_bodyRegisterBg, m_bodyReduceBg, m_bodyExtractBg,
        m_bodyCollideBg,   m_bodyStampBg})
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

  for (WGPUBuffer b :
       {m_voxBuf[0],        m_voxBuf[1],       m_brickBuf,     m_camBuf,
        m_frameBuf,         m_editBuf,         m_faceBuf,      m_anchorBuf[0],
        m_anchorBuf[1],     m_floodCtlBuf,     m_dirtyBuf,     m_labelBuf[0],
        m_labelBuf[1],      m_bodyBuf,         m_bodyXformBuf, m_slotMetaBuf,
        m_slotMetaReadback, m_rootSlotBuf,     m_slotCountBuf, m_freeSlotBuf,
        m_collideBuf,       m_collideReadback, m_tsResolve,    m_tsReadback})
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
  WGPUBindGroupLayoutEntry e[5] = {
      storageEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform),
      storageEntry(4, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 5);
  m_editPipe = makeComputePipeline(
      m_device, module, "edit", makePipelineLayout(m_device, bgl));

  for (int i = 0; i < 2; ++i)
  {
    WGPUBindGroupEntry be[5] = {bufEntry(0, m_voxBuf[i], m_voxBytes),
                                bufEntry(1, m_voxBuf[1 - i], m_voxBytes),
                                bufEntry(2, m_brickBuf, m_brickBytes),
                                bufEntry(3, m_editBuf, 32),
                                bufEntry(4, m_dirtyBuf, m_anchorBytes)};
    WGPUBindGroupDescriptor d = {};
    d.layout = bgl;
    d.entryCount = 5;
    d.entries = be;
    m_editBg[i] = wgpuDeviceCreateBindGroup(m_device, &d);
  }
}

void GpuVoxelWorld::buildRender()
{
  WGPUShaderModule module =
      makeShader(m_device, withCommon(shaders::voxelRenderWgsl).c_str());
  WGPUBindGroupLayoutEntry e[7] = {
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
          6, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 7);
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
    WGPUBindGroupEntry be[7] = {
        bufEntry(0, m_camBuf, 64),
        bufEntry(1, m_voxBuf[i], m_voxBytes),
        bufEntry(2, m_brickBuf, m_brickBytes),
        bufEntry(3, m_anchorBuf[0], m_anchorBytes),
        bufEntry(4, m_bodyBuf, m_bodyBytes * kMaxBodies),
        bufEntry(5, m_bodyXformBuf, 96 * kMaxBodies),
        bufEntry(6, m_labelBuf[0], m_anchorBytes)};
    WGPUBindGroupDescriptor bd = {};
    bd.layout = bgl;
    bd.entryCount = 7;
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
                              bufEntry(1, m_rootSlotBuf, m_anchorBytes),
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
                              bufEntry(3, m_rootSlotBuf, m_anchorBytes),
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
                              bufEntry(4, m_bodyBuf, m_bodyBytes * kMaxBodies),
                              bufEntry(5, m_labelBuf[0], m_anchorBytes),
                              bufEntry(6, m_rootSlotBuf, m_anchorBytes)};
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
  WGPUBindGroupLayoutEntry e[4] = {
      storageEntry(
          0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          1, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 4);
  m_bodyCollidePipe = makeComputePipeline(
      m_device, module, "collide", makePipelineLayout(m_device, bgl));

  WGPUBindGroupEntry be[4] = {bufEntry(0, m_bodyBuf, m_bodyBytes * kMaxBodies),
                              bufEntry(1, m_voxBuf[0], m_voxBytes),
                              bufEntry(2, m_bodyXformBuf, 96 * kMaxBodies),
                              bufEntry(3, m_collideBuf, 4 * kMaxBodies)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 4;
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

  WGPUBindGroupEntry be[4] = {bufEntry(0, m_bodyBuf, m_bodyBytes * kMaxBodies),
                              bufEntry(1, m_voxBuf[0], m_voxBytes),
                              bufEntry(2, m_voxBuf[1], m_voxBytes),
                              bufEntry(3, m_bodyXformBuf, 96 * kMaxBodies)};
  WGPUBindGroupDescriptor d = {};
  d.layout = bgl;
  d.entryCount = 4;
  d.entries = be;
  m_bodyStampBg = wgpuDeviceCreateBindGroup(m_device, &d);
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
  const float d = static_cast<float>(dt);
  for (int s = 0; s < kMaxBodies; ++s)
  {
    RigidBody& b = m_bodies[s];
    if (!b.active)
    {
      b.wasActive = false;
      continue;
    }
    if (!b.wasActive)
    {
      // Freshly extracted: upright and falling, not yet landed.
      b.wasActive = true;
      b.theta = 0.0f;
      b.omega = 0.0f;
      b.velY = 0.0f;
      b.landed = false;
      b.collided = false;
      b.resting = false;
    }
    if (b.resting)
      continue; // flat on the ground; waiting to be stamped into the world

    if (!b.landed)
    {
      // Fall straight down (gravity) until the body overlaps world solid.
      if (b.collided)
      {
        b.landed = true;
        b.omega = 0.4f; // ground contact -> start the topple
      }
      else
      {
        b.velY -= 80.0f * d;
        b.center[1] += b.velY * d;
      }
    }
    else if (b.theta < 1.5708f)
    {
      // Topple toward the CoM (inverted pendulum about the footprint) until
      // flat.
      b.omega += 2.5f * std::sin(b.theta) * d;
      b.theta += b.omega * d;
      if (b.theta > 1.5708f)
      {
        b.theta = 1.5708f;
        b.resting = true;
      }
    }
  }
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

  // Read back the per-slot body meta (one frame late) to place each chunk.
  if (m_bodyPendingResolve && !m_bodyMapBusy)
  {
    m_bodyMapBusy = true;
    m_bodyPendingResolve = false;
    WGPUBufferMapCallbackInfo mci = {};
    mci.mode = WGPUCallbackMode_AllowProcessEvents;
    mci.callback = onSlotMetaMapped;
    mci.userdata1 = &m_slotRb;
    wgpuBufferMapAsync(
        m_slotMetaReadback, WGPUMapMode_Read, 0, 64u * kMaxBodies, mci);
  }

  // Read back each slot's body-vs-world collision flag (one frame late).
  if (m_collidePendingResolve && !m_collideMapBusy)
  {
    m_collideMapBusy = true;
    m_collidePendingResolve = false;
    WGPUBufferMapCallbackInfo mci = {};
    mci.mode = WGPUCallbackMode_AllowProcessEvents;
    mci.callback = onCollideMapped;
    mci.userdata1 = &m_collideRb;
    wgpuBufferMapAsync(
        m_collideReadback, WGPUMapMode_Read, 0, 4u * kMaxBodies, mci);
  }

  wgpuQueueWriteBuffer(m_queue, m_camBuf, 0, cam16, 64);
  const uint32_t fdata[4] = {frame, 0, 0, 0};
  wgpuQueueWriteBuffer(m_queue, m_frameBuf, 0, fdata, sizeof(fdata));

  // Body transform array (one Body per pool slot): a world->local rotation
  // (transpose of the Rodrigues topple about the CoM axis) about the footprint
  // pivot. flags = (active, dim, resting/stamp-request).
  {
    float bu[24 * kMaxBodies] = {};
    for (int sb = 0; sb < kMaxBodies; ++sb)
    {
      const RigidBody& b = m_bodies[sb];
      float* p = bu + sb * 24;
      uint32_t* flags = reinterpret_cast<uint32_t*>(p);
      flags[0] = b.active ? 1u : 0u;
      flags[1] = static_cast<uint32_t>(kBodyDim);
      flags[2] = b.resting ? 1u : 0u;
      const float ax = b.axis[0];
      const float ay = b.axis[1];
      const float az = b.axis[2];
      const float c = std::cos(b.theta);
      const float s = std::sin(b.theta);
      const float t = 1.0f - c;
      p[4] = t * ax * ax + c;
      p[5] = t * ax * ay - s * az;
      p[6] = t * ax * az + s * ay;
      p[8] = t * ax * ay + s * az;
      p[9] = t * ay * ay + c;
      p[10] = t * ay * az - s * ax;
      p[12] = t * ax * az - s * ay;
      p[13] = t * ay * az + s * ax;
      p[14] = t * az * az + c;
      p[16] = b.center[0];
      p[17] = b.center[1];
      p[18] = b.center[2];
      p[20] = b.pivot[0];
      p[21] = b.pivot[1];
      p[22] = b.pivot[2];
    }
    wgpuQueueWriteBuffer(m_queue, m_bodyXformBuf, 0, bu, 96 * kMaxBodies);
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
    detectPass(m_facesPipe, m_facesBg, kBG, kBG, kBG);
    const uint32_t brickGroups = (kBG * kBG * kBG + 63) / 64;
    detectPass(m_seedPipe, m_seedBg, brickGroups, 1, 1);
    for (int round = 0; round < 64; ++round)
      detectPass(m_floodPipe, m_floodBg[round & 1], brickGroups, 1, 1);
    // Voxel-exact refinement at the carve boundary: only the bricks the edit
    // pass marked dirty do work; the rest early-out.
    detectPass(m_refinePipe, m_refineBg, kBG, kBG, kBG);
    // Label the detached set into connected components. No ground plane to
    // converge against, so run many rounds (a min walks one hop per round).
    detectPass(m_labelInitPipe, m_labelInitBg, brickGroups, 1, 1);
    for (int round = 0; round < 128; ++round)
      detectPass(
          m_labelFloodPipe, m_labelFloodBg[round & 1], brickGroups, 1, 1);
  }

  // Auto-fell: drop severed pieces the instant they are cut. The detect block
  // above (this same carve frame) has just labeled the freshly detached set, so
  // request a fell every carve frame.
  if (edit && edit->mode == 1)
    m_fellRequested = true;

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

  bool anyActive = false;
  bool anyResting = false;
  uint32_t freeList[kMaxBodies + 1] = {};
  uint32_t freeCount = 0;
  for (int s = 0; s < kMaxBodies; ++s)
  {
    if (m_bodies[s].active)
    {
      anyActive = true;
      if (m_bodies[s].resting)
        anyResting = true;
    }
    else
      freeList[1 + freeCount++] = static_cast<uint32_t>(s);
  }
  freeList[0] = freeCount;

  // Auto-felling: compact the labeled components into FREE body slots, extract
  // each chunk into its slot, clear it from the world, and re-anchor + re-label
  // the clean stumps. New cuts drop into free slots while earlier chunks are
  // still falling (no waiting for the pool to drain).
  if (m_fellRequested && freeCount > 0 && !m_bodyMapBusy)
  {
    m_fellRequested = false;
    wgpuQueueWriteBuffer(m_queue, m_freeSlotBuf, 0, freeList, sizeof(freeList));
    const uint32_t zeroCount = 0u;
    wgpuQueueWriteBuffer(m_queue, m_slotCountBuf, 0, &zeroCount, 4);
    // Per slot: aabbMin = +world (for atomicMin), everything else 0.
    int32_t metaInit[16 * kMaxBodies] = {};
    for (int s = 0; s < kMaxBodies; ++s)
    {
      metaInit[s * 16 + 0] = kWorld;
      metaInit[s * 16 + 1] = kWorld;
      metaInit[s * 16 + 2] = kWorld;
    }
    wgpuQueueWriteBuffer(m_queue, m_slotMetaBuf, 0, metaInit, sizeof(metaInit));
    bodyPass(m_bodyRegisterPipe, m_bodyRegisterBg, brickGroups, 1, 1);
    bodyPass(m_bodyReducePipe, m_bodyReduceBg, brickGroups, 1, 1);
    bodyPass(
        m_bodyExtractPipe, m_bodyExtractBg, kBodyDim / 4, kBodyDim / 4, bodyZ);
    // No re-anchor needed: extraction only removes voxels (now air -> never
    // tinted), and the stumps stay anchored from this carve's own detect pass.
    // The next carve (or a body's stamp) refreshes anchoring from scratch.
    wgpuCommandEncoderCopyBufferToBuffer(
        enc, m_slotMetaBuf, 0, m_slotMetaReadback, 0, 64u * kMaxBodies);
    m_bodyPendingResolve = true;
  }

  // While any body is active, test the whole pool against the world each frame
  // so the CPU sim knows when each falling body lands.
  if (anyActive && !m_collideMapBusy)
  {
    const uint32_t zeros[kMaxBodies] = {};
    wgpuQueueWriteBuffer(m_queue, m_collideBuf, 0, zeros, 4u * kMaxBodies);
    bodyPass(
        m_bodyCollidePipe, m_bodyCollideBg, kBodyDim / 4, kBodyDim / 4, bodyZ);
    wgpuCommandEncoderCopyBufferToBuffer(
        enc, m_collideBuf, 0, m_collideReadback, 0, 4u * kMaxBodies);
    m_collidePendingResolve = true;
  }

  // Each body that has finished toppling (flags.z set) stamps itself back into
  // the world; recount + re-anchor so the fallen logs read as grounded, then
  // free those slots.
  if (anyResting)
  {
    bodyPass(m_bodyStampPipe, m_bodyStampBg, kBodyDim / 4, kBodyDim / 4, bodyZ);
    bodyPass(m_recPipe, m_recBg[m_srcIdx], kBG, kBG, kBG);
    reanchorRelabel();
    for (int s = 0; s < kMaxBodies; ++s)
      if (m_bodies[s].resting)
      {
        m_bodies[s].active = false;
        m_bodies[s].resting = false;
      }
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
