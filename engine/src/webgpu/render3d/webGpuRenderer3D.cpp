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
  glm::vec4 sky;
  glm::uvec4 lightCount;
  std::array<GpuPointLight, kMaxPointLights3D> lights;
};

static_assert(kMaxPointLights3D == 32,
              "MAX_POINT_LIGHTS in mesh3DCommon.wgsl must match");
static_assert(sizeof(FrameUniforms) == 64 * 2 + 16 * 6 + 32 * 32,
              "FrameUniforms must match the WGSL Frame layout");
static_assert(sizeof(MeshVertex) == 44,
              "vertex layout below assumes 11 floats");
static_assert(sizeof(MeshInstance) == 80,
              "instance layout below assumes mat4 + vec4");

// Mirrors `Material` in mesh3DCommon.wgsl.
struct MaterialUniforms
{
  glm::vec4 baseColor;
  glm::vec4 params;      // unused, metallic, roughness, alpha cutoff
  glm::vec4 uvTransform; // tiling xy, offset zw
  glm::uvec4 mode;       // x: AlphaMode
};

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
  std::array<WGPUVertexAttribute, 4> vertexAttributes{{
      {WGPUVertexFormat_Float32x3, offsetof(MeshVertex, position), 0},
      {WGPUVertexFormat_Float32x3, offsetof(MeshVertex, normal), 1},
      {WGPUVertexFormat_Float32x3, offsetof(MeshVertex, color), 2},
      {WGPUVertexFormat_Float32x2, offsetof(MeshVertex, uv), 8},
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
  depth.depthWriteEnabled =
      write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
  depth.depthCompare = compare;
  depth.stencilFront.compare = WGPUCompareFunction_Always;
  depth.stencilFront.failOp = WGPUStencilOperation_Keep;
  depth.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
  depth.stencilFront.passOp = WGPUStencilOperation_Keep;
  depth.stencilBack = depth.stencilFront;
  return depth;
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

uint32_t mipLevels(uint32_t width, uint32_t height)
{
  uint32_t levels = 1;
  for (uint32_t size = std::max(width, height); size > 1; size /= 2)
    ++levels;
  return levels;
}

// The next mip level down: each texel the average of (up to) four. Averaged in
// sRGB rather than linear light; close enough for albedo.
std::vector<uint8_t> halve(const std::vector<uint8_t>& rgba,
                           uint32_t width,
                           uint32_t height,
                           uint32_t& outWidth,
                           uint32_t& outHeight)
{
  outWidth = std::max(width / 2, 1u);
  outHeight = std::max(height / 2, 1u);
  std::vector<uint8_t> out(std::size_t(outWidth) * outHeight * 4);
  for (uint32_t y = 0; y < outHeight; ++y)
    for (uint32_t x = 0; x < outWidth; ++x)
      for (uint32_t c = 0; c < 4; ++c)
      {
        const uint32_t x0 = std::min(x * 2, width - 1),
                       x1 = std::min(x * 2 + 1, width - 1);
        const uint32_t y0 = std::min(y * 2, height - 1),
                       y1 = std::min(y * 2 + 1, height - 1);
        const auto at = [&](uint32_t px, uint32_t py)
        { return rgba[(std::size_t(py) * width + px) * 4 + c]; };
        out[(std::size_t(y) * outWidth + x) * 4 + c] = static_cast<uint8_t>(
            (at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
      }
  return out;
}

void writeLevel(WGPUQueue queue,
                WGPUTexture texture,
                uint32_t level,
                const std::vector<uint8_t>& rgba,
                uint32_t width,
                uint32_t height)
{
  WGPUTexelCopyTextureInfo destination = {};
  destination.texture = texture;
  destination.mipLevel = level;
  destination.aspect = WGPUTextureAspect_All;
  WGPUTexelCopyBufferLayout layout = {};
  layout.bytesPerRow = width * 4;
  layout.rowsPerImage = height;
  const WGPUExtent3D size = {width, height, 1};
  wgpuQueueWriteTexture(
      queue, &destination, rgba.data(), rgba.size(), &layout, &size);
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
  m_frameUniforms =
      makeBuffer(m_context.device(),
                 sizeof(FrameUniforms),
                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  createShadowMap();

  if (!createTextureResources() || !createPipelines())
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
  gpu.vertices = makeBuffer(
      device, vertexBytes, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst);
  gpu.indices = makeBuffer(
      device, indexBytes, WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst);
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

  const auto addSection = [&](uint32_t firstIndex,
                              uint32_t indexCount,
                              const MeshMaterial& material)
  {
    // Clamp to the indices there are, so a bad section can't read past them.
    const uint32_t first = std::min(firstIndex, gpu.indexCount);
    const uint32_t count = std::min(indexCount, gpu.indexCount - first);
    std::optional<GpuMaterial> gpuMaterial = makeMaterial(material);
    // Even an empty section keeps its place, so slot numbers stay the mesh's.
    if (gpuMaterial)
      gpu.sections.push_back({first, count, std::move(*gpuMaterial)});
  };
  if (mesh.sections.empty())
    addSection(0, gpu.indexCount, MeshMaterial{});
  for (const MeshSection& section : mesh.sections)
    addSection(section.firstIndex, section.indexCount, section.material);
  if (gpu.sections.size() != std::max<std::size_t>(mesh.sections.size(), 1))
    return {};

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

MaterialHandle WebGpuRenderer3D::createMaterial(const MeshMaterial& material)
{
  std::optional<GpuMaterial> gpu = makeMaterial(material);
  if (!gpu)
    return {};

  uint32_t slot = 0;
  if (!m_freeMaterialSlots.empty())
  {
    slot = m_freeMaterialSlots.back();
    m_freeMaterialSlots.pop_back();
    m_materials[slot] = std::move(gpu);
  }
  else
  {
    slot = static_cast<uint32_t>(m_materials.size());
    m_materials.push_back(std::move(gpu));
  }
  return MaterialHandle{slot + 1};
}

void WebGpuRenderer3D::updateMaterial(MaterialHandle handle,
                                      const MeshMaterial& material)
{
  if (!handle.valid() || handle.id > m_materials.size() ||
      !m_materials[handle.id - 1])
    return;
  if (std::optional<GpuMaterial> gpu = makeMaterial(material))
    m_materials[handle.id - 1] = std::move(gpu);
}

void WebGpuRenderer3D::destroyMaterial(MaterialHandle material)
{
  if (!material.valid() || material.id > m_materials.size() ||
      !m_materials[material.id - 1])
    return;
  m_materials[material.id - 1].reset();
  m_freeMaterialSlots.push_back(material.id - 1);
}

void WebGpuRenderer3D::beginFrame(const FrameParams3D& frame)
{
  m_frame = frame;
  m_pointLights.assign(frame.pointLights.begin(), frame.pointLights.end());
  m_frame.pointLights = m_pointLights;

  m_instances.clear();
  m_drawMaterials.clear();
  m_litDraws.clear();
  m_blendDraws.clear();
  m_shadowDraws.clear();
  m_frameOpen = true;
}

void WebGpuRenderer3D::drawMesh(const MeshBatch3D& batch)
{
  const GpuMesh* mesh = m_frameOpen ? findMesh(batch.mesh) : nullptr;
  if (!mesh)
    return;

  // The batch's materials, shared by its draws.
  const auto firstMaterial = static_cast<uint32_t>(m_drawMaterials.size());
  const std::size_t materialCount =
      std::min(batch.materials.size(), mesh->sections.size());
  for (std::size_t i = 0; i < materialCount; ++i)
    m_drawMaterials.push_back(batch.materials[i].id);

  const uint32_t slot = batch.mesh.id - 1;
  const Draw base{
      slot, 0, 0, firstMaterial, static_cast<uint32_t>(materialCount), false};
  bool hasBlend = false;
  for (std::size_t i = 0; i < mesh->sections.size(); ++i)
    hasBlend =
        hasBlend || materialFor(*mesh, i, base).alphaMode == AlphaMode::Blend;

  const auto append = [&](const MeshInstance& instance)
  {
    m_instances.push_back(instance);
    return static_cast<uint32_t>(m_instances.size() - 1);
  };
  const auto seeThrough = [&](uint32_t index, bool allSections)
  {
    Draw draw = base;
    draw.firstInstance = index;
    draw.instanceCount = 1;
    draw.allSections = allSections;
    const glm::vec3 position(m_instances[index].model[3]);
    m_blendDraws.push_back(
        {draw, glm::distance(position, m_frame.cameraPosition)});
  };

  // Faded instances (tint alpha below 1) draw see-through, after everything
  // opaque; the rest draw together, but their Blend sections join the
  // see-through ones.
  Draw opaque = base;
  opaque.firstInstance = static_cast<uint32_t>(m_instances.size());
  std::vector<uint32_t> faded;
  for (const MeshInstance& instance : batch.instances)
    if (instance.tint.a < 1.0f)
      faded.push_back(append(instance));
  for (const MeshInstance& instance : batch.instances)
    if (instance.tint.a >= 1.0f)
    {
      const uint32_t index = append(instance);
      if (opaque.instanceCount == 0)
        opaque.firstInstance = index;
      ++opaque.instanceCount;
      if (hasBlend)
        seeThrough(index, false);
    }
  if (opaque.instanceCount > 0)
    m_litDraws.push_back(opaque);
  for (const uint32_t index : faded)
    seeThrough(index, true);

  if (m_frame.shadows.enabled && !batch.shadowInstances.empty())
  {
    Draw shadow = base;
    shadow.firstInstance = static_cast<uint32_t>(m_instances.size());
    shadow.instanceCount = static_cast<uint32_t>(batch.shadowInstances.size());
    m_shadowDraws.push_back(shadow);
    m_instances.insert(m_instances.end(),
                       batch.shadowInstances.begin(),
                       batch.shadowInstances.end());
  }
}

void WebGpuRenderer3D::endFrame()
{
  if (!m_frameOpen)
    return;
  m_frameOpen = false;

  if (!m_encoder || !m_target || m_viewport.x <= 0 || m_viewport.y <= 0)
    return;

  // Farthest first, so nearer see-through things blend over farther ones.
  std::stable_sort(m_blendDraws.begin(),
                   m_blendDraws.end(),
                   [](const BlendDraw& a, const BlendDraw& b)
                   { return a.distance > b.distance; });

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
  m_shadowView = makeDepthTexture(
      m_context.device(),
      size,
      WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
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

bool WebGpuRenderer3D::createTextureResources()
{
  const WGPUDevice device = m_context.device();

  // Group 1 of every mesh pipeline: the section's material.
  std::array<WGPUBindGroupLayoutEntry, 4> entries{};
  entries[0].binding = 0; // base colour texture
  entries[0].visibility = WGPUShaderStage_Fragment;
  entries[0].texture.sampleType = WGPUTextureSampleType_Float;
  entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
  entries[1].binding = 1; // its sampler
  entries[1].visibility = WGPUShaderStage_Fragment;
  entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
  entries[2].binding = 2; // metallic-roughness texture
  entries[2].visibility = WGPUShaderStage_Fragment;
  entries[2].texture.sampleType = WGPUTextureSampleType_Float;
  entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
  entries[3] = wgpu::storageEntry(
      3, WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform);
  WGPUBindGroupLayoutDescriptor layout = {};
  layout.entryCount = entries.size();
  layout.entries = entries.data();
  m_materialLayout.reset(wgpuDeviceCreateBindGroupLayout(device, &layout));

  // Textures repeat, as glTF's default wrap does.
  WGPUSamplerDescriptor sampler = {};
  sampler.addressModeU = WGPUAddressMode_Repeat;
  sampler.addressModeV = WGPUAddressMode_Repeat;
  sampler.addressModeW = WGPUAddressMode_Repeat;
  sampler.magFilter = WGPUFilterMode_Linear;
  sampler.minFilter = WGPUFilterMode_Linear;
  sampler.mipmapFilter = WGPUMipmapFilterMode_Linear;
  sampler.lodMinClamp = 0.0f;
  sampler.lodMaxClamp = 32.0f;
  sampler.maxAnisotropy = 8;
  m_linearSampler.reset(wgpuDeviceCreateSampler(device, &sampler));
  // Pixel art keeps hard texel edges up close, and still uses mipmaps far off.
  sampler.magFilter = WGPUFilterMode_Nearest;
  sampler.minFilter = WGPUFilterMode_Nearest;
  sampler.mipmapFilter = WGPUMipmapFilterMode_Nearest;
  sampler.maxAnisotropy = 1;
  m_nearestSampler.reset(wgpuDeviceCreateSampler(device, &sampler));

  // White stands in for a missing texture: colour x 1, and a metallic-
  // roughness of 1 leaves the material's factors as they are.
  auto white = std::make_shared<TextureData>();
  white->width = white->height = 1;
  white->rgba = {255, 255, 255, 255};
  m_white = uploadTexture(white);
  m_textures.clear();
  return m_materialLayout && m_linearSampler && m_nearestSampler && m_white;
}

std::shared_ptr<WebGpuRenderer3D::GpuTexture> WebGpuRenderer3D::uploadTexture(
    const std::shared_ptr<const TextureData>& texture)
{
  if (!texture || texture->empty() ||
      texture->rgba.size() < std::size_t(texture->width) * texture->height * 4)
    return nullptr;

  if (const auto it = m_textures.find(texture.get()); it != m_textures.end())
  {
    if (it->second.source.lock() == texture)
      if (std::shared_ptr<GpuTexture> live = it->second.gpu.lock())
        return live;
    m_textures.erase(it);
  }

  const WGPUDevice device = m_context.device();
  const uint32_t levels = mipLevels(texture->width, texture->height);
  WGPUTextureDescriptor desc = {};
  desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
  desc.dimension = WGPUTextureDimension_2D;
  desc.size = {texture->width, texture->height, 1};
  // Colour images are sRGB, so sampling gives linear colour; data stays as is.
  desc.format = texture->linear ? WGPUTextureFormat_RGBA8Unorm
                                : WGPUTextureFormat_RGBA8UnormSrgb;
  desc.mipLevelCount = levels;
  desc.sampleCount = 1;

  auto gpu = std::make_shared<GpuTexture>();
  gpu->nearest = texture->nearest;
  gpu->texture.reset(wgpuDeviceCreateTexture(device, &desc));
  if (!gpu->texture)
    return nullptr;

  uint32_t width = texture->width, height = texture->height;
  writeLevel(
      m_context.queue(), gpu->texture.get(), 0, texture->rgba, width, height);
  std::vector<uint8_t> level;
  const std::vector<uint8_t>* previous = &texture->rgba;
  for (uint32_t i = 1; i < levels; ++i)
  {
    uint32_t nextWidth = 0, nextHeight = 0;
    level = halve(*previous, width, height, nextWidth, nextHeight);
    width = nextWidth;
    height = nextHeight;
    writeLevel(m_context.queue(), gpu->texture.get(), i, level, width, height);
    previous = &level;
  }

  gpu->view.reset(wgpuTextureCreateView(gpu->texture.get(), nullptr));
  if (!gpu->view)
    return nullptr;

  // Forget entries whose meshes are all gone, so the map doesn't grow forever.
  std::erase_if(
      m_textures, [](const auto& entry) { return entry.second.gpu.expired(); });
  m_textures[texture.get()] = {texture, gpu};
  return gpu;
}

std::optional<WebGpuRenderer3D::GpuMaterial>
WebGpuRenderer3D::makeMaterial(const MeshMaterial& material)
{
  const WGPUDevice device = m_context.device();
  GpuMaterial gpu;
  gpu.alphaMode = material.alphaMode;
  gpu.baseTexture = uploadTexture(material.baseTexture);
  gpu.metallicRoughness = uploadTexture(material.metallicRoughnessTexture);

  const MaterialUniforms uniforms{
      material.baseColor,
      glm::vec4(
          0.0f, material.metallic, material.roughness, material.alphaCutoff),
      glm::vec4(material.tiling, material.offset),
      glm::uvec4(static_cast<uint32_t>(material.alphaMode), 0, 0, 0)};
  gpu.uniforms = makeBuffer(device,
                            sizeof(uniforms),
                            WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  if (!gpu.uniforms)
    return std::nullopt;
  wgpuQueueWriteBuffer(
      m_context.queue(), gpu.uniforms.get(), 0, &uniforms, sizeof(uniforms));

  const GpuTexture& base = gpu.baseTexture ? *gpu.baseTexture : *m_white;
  const GpuTexture& metallicRoughness =
      gpu.metallicRoughness ? *gpu.metallicRoughness : *m_white;
  std::array<WGPUBindGroupEntry, 4> entries{};
  entries[0].binding = 0;
  entries[0].textureView = base.view.get();
  entries[1].binding = 1;
  entries[1].sampler =
      base.nearest ? m_nearestSampler.get() : m_linearSampler.get();
  entries[2].binding = 2;
  entries[2].textureView = metallicRoughness.view.get();
  entries[3].binding = 3;
  entries[3].buffer = gpu.uniforms.get();
  entries[3].size = sizeof(MaterialUniforms);

  WGPUBindGroupDescriptor desc = {};
  desc.layout = m_materialLayout.get();
  desc.entryCount = entries.size();
  desc.entries = entries.data();
  gpu.bindings.reset(wgpuDeviceCreateBindGroup(device, &desc));
  if (!gpu.bindings)
    return std::nullopt;
  return gpu;
}

const WebGpuRenderer3D::GpuMaterial&
WebGpuRenderer3D::materialFor(const GpuMesh& mesh,
                              std::size_t index,
                              const Draw& draw) const
{
  if (index < draw.materialCount)
  {
    const uint32_t id = m_drawMaterials[draw.firstMaterial + index];
    if (id > 0 && id <= m_materials.size() && m_materials[id - 1])
      return *m_materials[id - 1];
  }
  return mesh.sections[index].material;
}

bool WebGpuRenderer3D::createPipelines()
{
  const WGPUDevice device = m_context.device();

  // Bind group layouts: the shadow pass reads only the frame uniforms; the
  // lit pass also samples the shadow map.
  const WGPUBindGroupLayoutEntry uniformEntry =
      wgpu::storageEntry(0,
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
  // Group 0: the frame (and, lit, the shadow map); group 1: the section's
  // material.
  const auto pipelineLayout = [&](WGPUBindGroupLayout frame)
  {
    const std::array<WGPUBindGroupLayout, 2> groups = {
        frame, m_materialLayout.get()};
    WGPUPipelineLayoutDescriptor desc = {};
    desc.bindGroupLayoutCount = groups.size();
    desc.bindGroupLayouts = groups.data();
    return wgpu::PipelineLayout(wgpuDeviceCreatePipelineLayout(device, &desc));
  };

  // Shadow pipeline: depth only, with a fragment stage that cuts out where a
  // masked or see-through material isn't solid. No culling, so open meshes
  // (planes, roof undersides) still cast; slope-scaled bias keeps sloped faces
  // clean.
  {
    const wgpu::ShaderModule module(wgpu::makeShader(
        device, withCommon(shaders::mesh3DShadowWgsl).c_str()));
    const wgpu::PipelineLayout layout = pipelineLayout(m_shadowLayout.get());

    WGPUDepthStencilState depth = depthState(true, WGPUCompareFunction_Less);
    depth.depthBias = 2;
    depth.depthBiasSlopeScale = 2.0f;

    WGPUFragmentState fragment = {};
    fragment.module = module.get();
    fragment.entryPoint = wgpu::sv("fs_main");
    fragment.targetCount = 0;

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
    desc.fragment = &fragment;
    m_shadowPipeline.reset(wgpuDeviceCreateRenderPipeline(device, &desc));
  }

  // Lit pipelines: back faces culled. Opaque ones test and write depth;
  // see-through ones test it, blend over what's there and leave depth alone.
  {
    const wgpu::ShaderModule module(
        wgpu::makeShader(device, withCommon(shaders::mesh3DLitWgsl).c_str()));
    const wgpu::PipelineLayout layout = pipelineLayout(m_litLayout.get());

    const auto build = [&](bool blend)
    {
      const WGPUDepthStencilState depth =
          depthState(!blend, WGPUCompareFunction_Less);

      // Colour blends by alpha; the target's alpha stays 1 (the editor shows
      // the viewport as an image, which a lower alpha would let show through).
      WGPUBlendState blending = {};
      blending.color = {WGPUBlendOperation_Add,
                        WGPUBlendFactor_SrcAlpha,
                        WGPUBlendFactor_OneMinusSrcAlpha};
      blending.alpha = {
          WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One};

      WGPUColorTargetState color = {};
      color.format = m_context.format();
      color.writeMask = WGPUColorWriteMask_All;
      color.blend = blend ? &blending : nullptr;

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
      return wgpu::RenderPipeline(
          wgpuDeviceCreateRenderPipeline(device, &desc));
    };
    m_litPipeline = build(false);
    m_blendPipeline = build(true);
  }

  return m_shadowLayout && m_litLayout && m_shadowBindings && m_litBindings &&
         m_shadowPipeline && m_litPipeline && m_blendPipeline;
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
  m_instanceBuffer =
      makeBuffer(m_context.device(),
                 m_instanceCapacity * sizeof(MeshInstance),
                 WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst);
}

void WebGpuRenderer3D::uploadFrame()
{
  const SceneLighting3D& lighting = m_frame.lighting;
  const ShadowSettings3D& shadows = m_frame.shadows;
  const float resolution = static_cast<float>(m_shadowResolution);

  const float sunLength = glm::length(lighting.sun.direction);
  const glm::vec3 toSun = sunLength > 1.0e-4f
                              ? lighting.sun.direction / sunLength
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
  uniforms.sky = glm::vec4(lighting.skyColor, 1.0f);

  const std::size_t lightCount =
      std::min(m_pointLights.size(), uniforms.lights.size());
  uniforms.lightCount = glm::uvec4(static_cast<uint32_t>(lightCount), 0, 0, 0);
  for (std::size_t i = 0; i < lightCount; ++i)
  {
    const PointLight3D& light = m_pointLights[i];
    uniforms.lights[i] = {glm::vec4(light.position, light.radius),
                          glm::vec4(light.color, light.intensity)};
  }

  wgpuQueueWriteBuffer(
      m_context.queue(), m_frameUniforms.get(), 0, &uniforms, sizeof(uniforms));

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

  WGPURenderPassEncoder pass =
      wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
  wgpuRenderPassEncoderSetPipeline(pass, m_shadowPipeline.get());
  wgpuRenderPassEncoderSetBindGroup(
      pass, 0, m_shadowBindings.get(), 0, nullptr);
  if (!m_shadowDraws.empty())
    wgpuRenderPassEncoderSetVertexBuffer(
        pass, 1, m_instanceBuffer.get(), 0, WGPU_WHOLE_SIZE);
  for (const Draw& draw : m_shadowDraws)
    encodeDraw(pass, draw, Sections::All);
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

  WGPURenderPassEncoder pass =
      wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
  if (!m_instances.empty())
    wgpuRenderPassEncoderSetVertexBuffer(
        pass, 1, m_instanceBuffer.get(), 0, WGPU_WHOLE_SIZE);

  wgpuRenderPassEncoderSetPipeline(pass, m_litPipeline.get());
  wgpuRenderPassEncoderSetBindGroup(pass, 0, m_litBindings.get(), 0, nullptr);
  for (const Draw& draw : m_litDraws)
    encodeDraw(pass, draw, Sections::Opaque);

  if (!m_blendDraws.empty())
  {
    wgpuRenderPassEncoderSetPipeline(pass, m_blendPipeline.get());
    wgpuRenderPassEncoderSetBindGroup(pass, 0, m_litBindings.get(), 0, nullptr);
    for (const BlendDraw& blend : m_blendDraws)
      encodeDraw(pass, blend.draw, Sections::SeeThrough);
  }
  endPass(pass);
}

void WebGpuRenderer3D::encodeDraw(WGPURenderPassEncoder pass,
                                  const Draw& draw,
                                  Sections which)
{
  // The mesh may have been destroyed between drawMesh() and endFrame().
  const std::optional<GpuMesh>& mesh = m_meshes[draw.meshSlot];
  if (!mesh)
    return;

  wgpuRenderPassEncoderSetVertexBuffer(
      pass, 0, mesh->vertices.get(), 0, WGPU_WHOLE_SIZE);
  wgpuRenderPassEncoderSetIndexBuffer(
      pass, mesh->indices.get(), WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);

  for (std::size_t i = 0; i < mesh->sections.size(); ++i)
  {
    const GpuSection& section = mesh->sections[i];
    const GpuMaterial& material = materialFor(*mesh, i, draw);
    const bool blend = material.alphaMode == AlphaMode::Blend;
    if (section.indexCount == 0 || (which == Sections::Opaque && blend) ||
        (which == Sections::SeeThrough && !blend && !draw.allSections))
      continue;
    wgpuRenderPassEncoderSetBindGroup(
        pass, 1, material.bindings.get(), 0, nullptr);
    wgpuRenderPassEncoderDrawIndexed(pass,
                                     section.indexCount,
                                     draw.instanceCount,
                                     section.firstIndex,
                                     0,
                                     draw.firstInstance);
  }
}

const WebGpuRenderer3D::GpuMesh*
WebGpuRenderer3D::findMesh(MeshHandle mesh) const
{
  if (!mesh.valid() || mesh.id > m_meshes.size())
    return nullptr;

  const std::optional<GpuMesh>& slot = m_meshes[mesh.id - 1];
  return slot ? &*slot : nullptr;
}

} // namespace sfs
