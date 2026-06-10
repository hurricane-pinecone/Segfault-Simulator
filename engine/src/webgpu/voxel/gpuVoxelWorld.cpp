#include "engine/webgpu/voxel/gpuVoxelWorld.h"

#include "engine/webgpu/wgpuUtil.h"

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

  buildTimestamps();
  buildGenerate();
  buildWater();
  buildRecount();
  buildEdit();
  buildRender();
  buildFaces();
  buildAnchor();
  buildRefine();
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
                           m_refineBg})
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

  for (WGPUBuffer b : {m_voxBuf[0],
                       m_voxBuf[1],
                       m_brickBuf,
                       m_camBuf,
                       m_frameBuf,
                       m_editBuf,
                       m_faceBuf,
                       m_anchorBuf[0],
                       m_anchorBuf[1],
                       m_floodCtlBuf,
                       m_dirtyBuf,
                       m_tsResolve,
                       m_tsReadback})
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
  WGPUBindGroupLayoutEntry e[4] = {
      storageEntry(0, WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform),
      storageEntry(
          1, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          2, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage),
      storageEntry(
          3, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage)};
  WGPUBindGroupLayout bgl = makeBgl(m_device, e, 4);
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
    WGPUBindGroupEntry be[4] = {bufEntry(0, m_camBuf, 64),
                                bufEntry(1, m_voxBuf[i], m_voxBytes),
                                bufEntry(2, m_brickBuf, m_brickBytes),
                                bufEntry(3, m_anchorBuf[0], m_anchorBytes)};
    WGPUBindGroupDescriptor bd = {};
    bd.layout = bgl;
    bd.entryCount = 4;
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

  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(m_queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);
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

  wgpuQueueWriteBuffer(m_queue, m_camBuf, 0, cam16, 64);
  const uint32_t fdata[4] = {frame, 0, 0, 0};
  wgpuQueueWriteBuffer(m_queue, m_frameBuf, 0, fdata, sizeof(fdata));

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
