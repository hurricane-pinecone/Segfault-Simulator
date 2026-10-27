#include "engine/webgpu/render3d/webGpuRenderer3D.h"

#include "engine/core/logger/logger.h"
#include "engine/webgpu/webGpuContext.h"
#include "engine/webgpu/wgpuUtil.h"
#include "glm/glm/ext/vector_uint4.hpp"
#include "glm/glm/geometric.hpp"

#include <engine/generated/embeddedWgsl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

namespace sfs
{

namespace
{

constexpr WGPUTextureFormat kDepthFormat = WGPUTextureFormat_Depth32Float;

// Mirrors `Frame` in mesh3DCommon.wgsl (uniform layout: 16-byte aligned).
struct GpuPointLight
{
  glm::vec4 positionRadius;
  glm::vec4 colorIntensity;
};

struct FrameUniforms
{
  glm::mat4 viewProjection;
  glm::mat4 lightViewProjection;
  glm::vec4 cameraPosition;
  glm::vec4 sunDirection;
  glm::vec4 sunColor;
  glm::vec4 shadow;
  glm::uvec4 lightCount;
  std::array<GpuPointLight, kMaxPointLights3D> lights;
};

static_assert(kMaxPointLights3D == 32,
              "MAX_POINT_LIGHTS in mesh3DCommon.wgsl must match");
static_assert(sizeof(FrameUniforms) == 64 * 2 + 16 * 5 + 32 * 32,
              "FrameUniforms must match the WGSL Frame layout");
static_assert(sizeof(MeshVertex) == 36, "vertex layout below assumes 9 floats");
static_assert(sizeof(MeshInstance) == 80,
              "instance layout below assumes mat4 + vec4");

std::string withCommon(std::string_view shader)
{
  std::string source(shaders::mesh3DCommonWgsl);
  source += shader;
  return source;
}

wgpu::Buffer makeBuffer(WGPUDevice device, uint64_t size, WGPUBufferUsage usage)
{
  return wgpu::Buffer(wgpu::makeBuffer(device, size, usage));
}

// The two vertex buffers every mesh pipeline reads: per-vertex mesh data and
// per-instance model matrix + tint.
struct MeshVertexLayouts
{
  std::array<WGPUVertexAttribute, 3> vertexAttributes{{
      {WGPUVertexFormat_Float32x3, offsetof(MeshVertex, position), 0},
      {WGPUVertexFormat_Float32x3, offsetof(MeshVertex, normal), 1},
      {WGPUVertexFormat_Float32x3, offsetof(MeshVertex, color), 2},
  }};
  std::array<WGPUVertexAttribute, 5> instanceAttributes{{
      {WGPUVertexFormat_Float32x4, offsetof(MeshInstance, model) + 0, 3},
      {WGPUVertexFormat_Float32x4, offsetof(MeshInstance, model) + 16, 4},
      {WGPUVertexFormat_Float32x4, offsetof(MeshInstance, model) + 32, 5},
      {WGPUVertexFormat_Float32x4, offsetof(MeshInstance, model) + 48, 6},
      {WGPUVertexFormat_Float32x4, offsetof(MeshInstance, tint), 7},
  }};
  std::array<WGPUVertexBufferLayout, 2> buffers{};

  MeshVertexLayouts()
  {
    buffers[0].stepMode = WGPUVertexStepMode_Vertex;
    buffers[0].arrayStride = sizeof(MeshVertex);
    buffers[0].attributeCount = vertexAttributes.size();
    buffers[0].attributes = vertexAttributes.data();

    buffers[1].stepMode = WGPUVertexStepMode_Instance;
    buffers[1].arrayStride = sizeof(MeshInstance);
    buffers[1].attributeCount = instanceAttributes.size();
    buffers[1].attributes = instanceAttributes.data();
  }

  MeshVertexLayouts(const MeshVertexLayouts&) = delete;
  MeshVertexLayouts& operator=(const MeshVertexLayouts&) = delete;
};

WGPUDepthStencilState depthState(bool write, WGPUCompareFunction compare)
{
  WGPUDepthStencilState depth = {};
  depth.format = kDepthFormat;
  depth.depthWriteEnabled = write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
  depth.depthCompare = compare;
  depth.stencilFront.compare = WGPUCompareFunction_Always;
  depth.stencilFront.failOp = WGPUStencilOperation_Keep;
  depth.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
  depth.stencilFront.passOp = WGPUStencilOperation_Keep;
  depth.stencilBack = depth.stencilFront;
  return depth;
}

wgpu::PipelineLayout makePipelineLayout(WGPUDevice device,
                                        WGPUBindGroupLayout bindings)
{
  WGPUPipelineLayoutDescriptor desc = {};
  desc.bindGroupLayoutCount = 1;
  desc.bindGroupLayouts = &bindings;
  return wgpu::PipelineLayout(wgpuDeviceCreatePipelineLayout(device, &desc));
}

wgpu::TextureView makeDepthTexture(WGPUDevice device,
                                   glm::ivec2 size,
                                   WGPUTextureUsage usage,
                                   wgpu::Texture& outTexture)
{
  WGPUTextureDescriptor desc = {};
  desc.usage = usage;
  desc.dimension = WGPUTextureDimension_2D;
  desc.size = {static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), 1};
  desc.format = kDepthFormat;
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;
  outTexture.reset(wgpuDeviceCreateTexture(device, &desc));
  return wgpu::TextureView(wgpuTextureCreateView(outTexture.get(), nullptr));
}

void endPass(WGPURenderPassEncoder pass)
{
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
}

} // namespace

WebGpuRenderer3D::WebGpuRenderer3D(WebGpuContext& context,
                                   uint32_t shadowMapResolution)
    : m_context(context), m_shadowResolution(std::max(shadowMapResolution, 1u))
{
}

WebGpuRenderer3D::~WebGpuRenderer3D() = default;

bool WebGpuRenderer3D::initialize()
{
  m_frameUniforms = makeBuffer(m_context.device(),
                               sizeof(FrameUniforms),
                               WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  createShadowMap();

  if (!createPipelines())
  {
    LOG_ERROR("WebGpuRenderer3D: failed to build render pipelines");
    return false;
  }
  return true;
}

void WebGpuRenderer3D::setTarget(WGPUCommandEncoder encoder,
                                 WGPUTextureView target,
                                 glm::ivec2 size)
{
  m_encoder = encoder;
  m_target = target;
  m_viewport = size;
}

MeshHandle WebGpuRenderer3D::createMesh(const MeshData& mesh)
{
  if (mesh.empty())
    return {};

  const WGPUDevice device = m_context.device();
  const uint64_t vertexBytes = mesh.vertices.size() * sizeof(MeshVertex);
  const uint64_t indexBytes = mesh.indices.size() * sizeof(uint32_t);

  GpuMesh gpu;
  gpu.vertices = makeBuffer(device,
                            vertexBytes,
                            WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst);
  gpu.indices = makeBuffer(device,
                           indexBytes,
                           WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst);
  if (!gpu.vertices || !gpu.indices)
    return {};

  wgpuQueueWriteBuffer(m_context.queue(),
                       gpu.vertices.get(),
                       0,
                       mesh.vertices.data(),
                       vertexBytes);
  wgpuQueueWriteBuffer(
      m_context.queue(), gpu.indices.get(), 0, mesh.indices.data(), indexBytes);
  gpu.indexCount = static_cast<uint32_t>(mesh.indices.size());

  uint32_t slot = 0;
  if (!m_freeSlots.empty())
  {
    slot = m_freeSlots.back();
    m_freeSlots.pop_back();
    m_meshes[slot] = std::move(gpu);
  }
  else
  {
    slot = static_cast<uint32_t>(m_meshes.size());
    m_meshes.emplace_back(std::move(gpu));
  }

  return MeshHandle{slot + 1};
}

void WebGpuRenderer3D::destroyMesh(MeshHandle mesh)
{
  if (!findMesh(mesh))
    return;

  const uint32_t slot = mesh.id - 1;
  m_meshes[slot].reset();
  m_freeSlots.push_back(slot);
}

void WebGpuRenderer3D::beginFrame(const FrameParams3D& frame)
{
  m_frame = frame;
  m_pointLights.assign(frame.pointLights.begin(), frame.pointLights.end());
  m_frame.pointLights = m_pointLights;

  m_instances.clear();
  m_litDraws.clear();
  m_shadowDraws.clear();
  m_frameOpen = true;
}

void WebGpuRenderer3D::drawMesh(const MeshBatch3D& batch)
{
  if (!m_frameOpen || !findMesh(batch.mesh))
    return;

  const uint32_t slot = batch.mesh.id - 1;
  const auto append = [&](std::span<const MeshInstance> instances,
                          std::vector<Draw>& draws)
  {
    if (instances.empty())
      return;
    draws.push_back({slot,
                     static_cast<uint32_t>(m_instances.size()),
                     static_cast<uint32_t>(instances.size())});
    m_instances.insert(m_instances.end(), instances.begin(), instances.end());
  };

  append(batch.instances, m_litDraws);
  if (m_frame.shadows.enabled)
    append(batch.shadowInstances, m_shadowDraws);
}

void WebGpuRenderer3D::endFrame()
{
  if (!m_frameOpen)
    return;
  m_frameOpen = false;

  if (!m_encoder || !m_target || m_viewport.x <= 0 || m_viewport.y <= 0)
    return;

  ensureDepthTarget();
  uploadFrame();

  if (m_frame.shadows.enabled)
    encodeShadowPass();
  encodeLitPass();
}

void WebGpuRenderer3D::createShadowMap()
{
  const glm::ivec2 size{static_cast<int>(m_shadowResolution),
                        static_cast<int>(m_shadowResolution)};
  m_shadowView = makeDepthTexture(m_context.device(),
                                  size,
                                  WGPUTextureUsage_RenderAttachment |
                                      WGPUTextureUsage_TextureBinding,
                                  m_shadowMap);

  // Comparison sampling with linear filtering: each tap is a hardware 2x2
  // percentage-closer filter.
  WGPUSamplerDescriptor desc = {};
  desc.addressModeU = WGPUAddressMode_ClampToEdge;
  desc.addressModeV = WGPUAddressMode_ClampToEdge;
  desc.addressModeW = WGPUAddressMode_ClampToEdge;
  desc.magFilter = WGPUFilterMode_Linear;
  desc.minFilter = WGPUFilterMode_Linear;
  desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
  desc.lodMinClamp = 0.0f;
  desc.lodMaxClamp = 1.0f;
  desc.compare = WGPUCompareFunction_LessEqual;
  desc.maxAnisotropy = 1;
  m_shadowSampler.reset(wgpuDeviceCreateSampler(m_context.device(), &desc));
}

bool WebGpuRenderer3D::createPipelines()
{
  const WGPUDevice device = m_context.device();

  // Bind group layouts: the shadow pass reads only the frame uniforms; the
  // lit pass also samples the shadow map.
  const WGPUBindGroupLayoutEntry uniformEntry = wgpu::storageEntry(
      0,
      WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
      WGPUBufferBindingType_Uniform);

  {
    WGPUBindGroupLayoutDescriptor desc = {};
    desc.entryCount = 1;
    desc.entries = &uniformEntry;
    m_shadowLayout.reset(wgpuDeviceCreateBindGroupLayout(device, &desc));
  }
  {
    std::array<WGPUBindGroupLayoutEntry, 3> entries{};
    entries[0] = uniformEntry;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].texture.sampleType = WGPUTextureSampleType_Depth;
    entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Fragment;
    entries[2].sampler.type = WGPUSamplerBindingType_Comparison;

    WGPUBindGroupLayoutDescriptor desc = {};
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    m_litLayout.reset(wgpuDeviceCreateBindGroupLayout(device, &desc));
  }

  // Bind groups (fixed for the renderer's lifetime: the uniform buffer and
  // shadow map never change).
  {
    WGPUBindGroupEntry entry = {};
    entry.binding = 0;
    entry.buffer = m_frameUniforms.get();
    entry.size = sizeof(FrameUniforms);

    WGPUBindGroupDescriptor desc = {};
    desc.layout = m_shadowLayout.get();
    desc.entryCount = 1;
    desc.entries = &entry;
    m_shadowBindings.reset(wgpuDeviceCreateBindGroup(device, &desc));
  }
  {
    std::array<WGPUBindGroupEntry, 3> entries{};
    entries[0].binding = 0;
    entries[0].buffer = m_frameUniforms.get();
    entries[0].size = sizeof(FrameUniforms);
    entries[1].binding = 1;
    entries[1].textureView = m_shadowView.get();
    entries[2].binding = 2;
    entries[2].sampler = m_shadowSampler.get();

    WGPUBindGroupDescriptor desc = {};
    desc.layout = m_litLayout.get();
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    m_litBindings.reset(wgpuDeviceCreateBindGroup(device, &desc));
  }

  const MeshVertexLayouts layouts;

  // Shadow pipeline: depth only. No culling, so open meshes (planes, roof
  // undersides) still cast; slope-scaled bias keeps sloped faces clean.
  {
    const wgpu::ShaderModule module(wgpu::makeShader(
        device, withCommon(shaders::mesh3DShadowWgsl).c_str()));
    const wgpu::PipelineLayout layout =
        makePipelineLayout(device, m_shadowLayout.get());

    WGPUDepthStencilState depth = depthState(true, WGPUCompareFunction_Less);
    depth.depthBias = 2;
    depth.depthBiasSlopeScale = 2.0f;

    WGPURenderPipelineDescriptor desc = {};
    desc.layout = layout.get();
    desc.vertex.module = module.get();
    desc.vertex.entryPoint = wgpu::sv("vs_main");
    desc.vertex.bufferCount = layouts.buffers.size();
    desc.vertex.buffers = layouts.buffers.data();
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.primitive.frontFace = WGPUFrontFace_CCW;
    desc.primitive.cullMode = WGPUCullMode_None;
    desc.depthStencil = &depth;
    desc.multisample.count = 1;
    desc.multisample.mask = 0xFFFFFFFFu;
    m_shadowPipeline.reset(wgpuDeviceCreateRenderPipeline(device, &desc));
  }

  // Lit pipeline: back faces culled, depth tested and written.
  {
    const wgpu::ShaderModule module(
        wgpu::makeShader(device, withCommon(shaders::mesh3DLitWgsl).c_str()));
    const wgpu::PipelineLayout layout =
        makePipelineLayout(device, m_litLayout.get());

    const WGPUDepthStencilState depth =
        depthState(true, WGPUCompareFunction_Less);

    WGPUColorTargetState color = {};
    color.format = m_context.format();
    color.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment = {};
    fragment.module = module.get();
    fragment.entryPoint = wgpu::sv("fs_main");
    fragment.targetCount = 1;
    fragment.targets = &color;

    WGPURenderPipelineDescriptor desc = {};
    desc.layout = layout.get();
    desc.vertex.module = module.get();
    desc.vertex.entryPoint = wgpu::sv("vs_main");
    desc.vertex.bufferCount = layouts.buffers.size();
    desc.vertex.buffers = layouts.buffers.data();
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.primitive.frontFace = WGPUFrontFace_CCW;
    desc.primitive.cullMode = WGPUCullMode_Back;
    desc.depthStencil = &depth;
    desc.multisample.count = 1;
    desc.multisample.mask = 0xFFFFFFFFu;
    desc.fragment = &fragment;
    m_litPipeline.reset(wgpuDeviceCreateRenderPipeline(device, &desc));
  }

  return m_shadowLayout && m_litLayout && m_shadowBindings && m_litBindings &&
         m_shadowPipeline && m_litPipeline;
}

void WebGpuRenderer3D::ensureDepthTarget()
{
  if (m_depth && m_depthSize == m_viewport)
    return;

  m_depthView = makeDepthTexture(m_context.device(),
                                 m_viewport,
                                 WGPUTextureUsage_RenderAttachment,
                                 m_depth);
  m_depthSize = m_viewport;
}

void WebGpuRenderer3D::ensureInstanceCapacity(std::size_t instanceCount)
{
  if (instanceCount <= m_instanceCapacity)
    return;

  // Grow geometrically so a scene that keeps adding objects reallocates
  // rarely.
  m_instanceCapacity =
      std::max({instanceCount, m_instanceCapacity * 2, std::size_t{256}});
  m_instanceBuffer = makeBuffer(m_context.device(),
                                m_instanceCapacity * sizeof(MeshInstance),
                                WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst);
}

void WebGpuRenderer3D::uploadFrame()
{
  const SceneLighting3D& lighting = m_frame.lighting;
  const ShadowSettings3D& shadows = m_frame.shadows;
  const float resolution = static_cast<float>(m_shadowResolution);

  const float sunLength = glm::length(lighting.sun.direction);
  const glm::vec3 toSun = sunLength > 1.0e-4f ? lighting.sun.direction / sunLength
                                              : glm::vec3(0.0f, 0.0f, 1.0f);

  FrameUniforms uniforms = {};
  uniforms.viewProjection = m_frame.viewProjection;
  uniforms.lightViewProjection = m_frame.lightViewProjection;
  uniforms.cameraPosition = glm::vec4(m_frame.cameraPosition, 1.0f);
  uniforms.sunDirection = glm::vec4(toSun, lighting.sun.diffuseStrength);
  uniforms.sunColor = glm::vec4(lighting.sun.color, lighting.ambient);
  uniforms.shadow = glm::vec4(shadows.enabled ? 1.0f : 0.0f,
                              shadows.depthBias,
                              shadows.softness / resolution,
                              2.0f * shadows.distance / resolution);

  const std::size_t lightCount =
      std::min(m_pointLights.size(), uniforms.lights.size());
  uniforms.lightCount = glm::uvec4(static_cast<uint32_t>(lightCount), 0, 0, 0);
  for (std::size_t i = 0; i < lightCount; ++i)
  {
    const PointLight3D& light = m_pointLights[i];
    uniforms.lights[i] = {glm::vec4(light.position, light.radius),
                          glm::vec4(light.color, light.intensity)};
  }

  wgpuQueueWriteBuffer(m_context.queue(),
                       m_frameUniforms.get(),
                       0,
                       &uniforms,
                       sizeof(uniforms));

  if (m_instances.empty())
    return;

  ensureInstanceCapacity(m_instances.size());
  wgpuQueueWriteBuffer(m_context.queue(),
                       m_instanceBuffer.get(),
                       0,
                       m_instances.data(),
                       m_instances.size() * sizeof(MeshInstance));
}

void WebGpuRenderer3D::encodeShadowPass()
{
  WGPURenderPassDepthStencilAttachment depth = {};
  depth.view = m_shadowView.get();
  depth.depthLoadOp = WGPULoadOp_Clear;
  depth.depthStoreOp = WGPUStoreOp_Store;
  depth.depthClearValue = 1.0f;

  WGPURenderPassDescriptor desc = {};
  desc.depthStencilAttachment = &depth;

  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
  wgpuRenderPassEncoderSetPipeline(pass, m_shadowPipeline.get());
  wgpuRenderPassEncoderSetBindGroup(pass, 0, m_shadowBindings.get(), 0, nullptr);
  encodeDraws(pass, m_shadowDraws);
  endPass(pass);
}

void WebGpuRenderer3D::encodeLitPass()
{
  const glm::vec3& sky = m_frame.lighting.skyColor;

  WGPURenderPassColorAttachment color = {};
  color.view = m_target;
  color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  color.loadOp = WGPULoadOp_Clear;
  color.storeOp = WGPUStoreOp_Store;
  color.clearValue = WGPUColor{sky.r, sky.g, sky.b, 1.0};

  WGPURenderPassDepthStencilAttachment depth = {};
  depth.view = m_depthView.get();
  depth.depthLoadOp = WGPULoadOp_Clear;
  depth.depthStoreOp = WGPUStoreOp_Discard;
  depth.depthClearValue = 1.0f;

  WGPURenderPassDescriptor desc = {};
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color;
  desc.depthStencilAttachment = &depth;

  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
  wgpuRenderPassEncoderSetPipeline(pass, m_litPipeline.get());
  wgpuRenderPassEncoderSetBindGroup(pass, 0, m_litBindings.get(), 0, nullptr);
  encodeDraws(pass, m_litDraws);
  endPass(pass);
}

void WebGpuRenderer3D::encodeDraws(WGPURenderPassEncoder pass,
                                   const std::vector<Draw>& draws)
{
  if (draws.empty())
    return;

  wgpuRenderPassEncoderSetVertexBuffer(
      pass, 1, m_instanceBuffer.get(), 0, WGPU_WHOLE_SIZE);

  for (const Draw& draw : draws)
  {
    // The mesh may have been destroyed between drawMesh() and endFrame().
    const std::optional<GpuMesh>& mesh = m_meshes[draw.meshSlot];
    if (!mesh)
      continue;

    wgpuRenderPassEncoderSetVertexBuffer(
        pass, 0, mesh->vertices.get(), 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderSetIndexBuffer(
        pass, mesh->indices.get(), WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderDrawIndexed(
        pass, mesh->indexCount, draw.instanceCount, 0, 0, draw.firstInstance);
  }
}

const WebGpuRenderer3D::GpuMesh* WebGpuRenderer3D::findMesh(MeshHandle mesh) const
{
  if (!mesh.valid() || mesh.id > m_meshes.size())
    return nullptr;

  const std::optional<GpuMesh>& slot = m_meshes[mesh.id - 1];
  return slot ? &*slot : nullptr;
}

} // namespace sfs
