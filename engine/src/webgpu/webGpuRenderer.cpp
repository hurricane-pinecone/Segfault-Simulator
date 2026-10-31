#include "engine/webgpu/webGpuRenderer.h"

#include "engine/core/logger/logger.h"
#include "engine/core/rendering/materialGraphCompiler.h"
#include "engine/webgpu/webGpuContext.h"
#include "engine/webgpu/wgpuUtil.h"
#include "glm/glm/ext/matrix_clip_space.hpp"
#include "glm/glm/ext/scalar_constants.hpp"
#include "glm/glm/ext/vector_int4.hpp"
#include "glm/glm/ext/vector_uint4.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/gtc/constants.hpp"
#include "glm/glm/gtc/packing.hpp"
#include "glm/glm/matrix.hpp"
#include "glm/glm/trigonometric.hpp"

#include "shaders/meshShaders.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>

namespace sfs
{

namespace
{

constexpr WGPUTextureFormat kDepthFormat = WGPUTextureFormat_Depth32Float;
// The lit scene and bloom: HDR, so light above 1 survives to bloom.
constexpr WGPUTextureFormat kSceneFormat = WGPUTextureFormat_RGBA16Float;
// Samples a pixel the lit pass draws with (multisampled anti-aliasing: smooth
// edges and thin grass), resolved into the scene's textures after it.
constexpr uint32_t kSceneSamplesMost = 4;
// Bloom's levels: up to this many, each half the one before, while they're
// at least kBloomSmallest pixels.
constexpr int kBloomLevels = 6;
constexpr int kBloomSmallest = 8;

// Each point-shadow face's uniforms sit at a multiple of kFaceStride in one
// buffer (a dynamic offset's alignment); so do the sun's cascades'.
constexpr uint64_t kFaceStride = 256;
static_assert(sizeof(FaceUniforms) <= kFaceStride);

// A physical sky's textures: the sky (octahedral, the whole sphere), its
// reflections (mip 0 sharp, each next rougher, to roughness 1 at the last)
// and the BRDF table; all half floats.
constexpr WGPUTextureFormat kSkyFormat = WGPUTextureFormat_RGBA16Float;
constexpr uint32_t kSkySize = 256;
constexpr uint32_t kSkyReflectionSize = 128;
constexpr uint32_t kSkyReflectionMips = 5;
constexpr int kBrdfTableSize = 32;
// Samples each reflection mip's texels take of the sky (rougher: more).
constexpr std::array<float, kSkyReflectionMips> kSkyReflectionSamples =
    {1.0f, 64.0f, 128.0f, 256.0f, 256.0f};

static_assert(sizeof(MeshVertex) == 44,
              "vertex layout below assumes 11 floats");
static_assert(sizeof(MeshInstance) == 96,
              "instance layout below assumes mat4 + vec4 + vec4");

// Errors raised between a ValidationScope's construction and end(): how
// WebGPU reports a shader or pipeline that doesn't build.
class ValidationScope
{
public:
  explicit ValidationScope(WGPUDevice device) : m_device(device)
  {
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
  }

  /** The first error's message; nothing if there was none. */
  std::optional<std::string> end()
  {
    std::optional<std::string> error;
    WGPUPopErrorScopeCallbackInfo callback = {};
    // wgpu-native answers before returning, as it does adapter and device
    // requests (see WebGpuContext).
    callback.mode = WGPUCallbackMode_AllowProcessEvents;
    callback.callback = [](WGPUPopErrorScopeStatus status,
                           WGPUErrorType type,
                           WGPUStringView message,
                           void* out,
                           void*)
    {
      if (status == WGPUPopErrorScopeStatus_Success &&
          type != WGPUErrorType_NoError)
        *static_cast<std::optional<std::string>*>(out) =
            std::string(message.data ? message.data : "",
                        message.data ? message.length : 0);
    };
    callback.userdata1 = &error;
    wgpuDevicePopErrorScope(m_device, callback);
    return error;
  }

private:
  WGPUDevice m_device;
};

// A material graph's group 1: its parameters, then each texture and its
// sampler.
wgpu::BindGroupLayout graphMaterialLayout(WGPUDevice device,
                                          std::size_t textureCount)
{
  std::vector<WGPUBindGroupLayoutEntry> entries;
  entries.push_back(wgpu::storageEntry(
      0, WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform));
  for (std::size_t i = 0; i < textureCount; ++i)
  {
    WGPUBindGroupLayoutEntry texture = {};
    texture.binding = static_cast<uint32_t>(1 + 2 * i);
    texture.visibility = WGPUShaderStage_Fragment;
    texture.texture.sampleType = WGPUTextureSampleType_Float;
    texture.texture.viewDimension = WGPUTextureViewDimension_2D;
    entries.push_back(texture);
    WGPUBindGroupLayoutEntry sampler = {};
    sampler.binding = static_cast<uint32_t>(2 + 2 * i);
    sampler.visibility = WGPUShaderStage_Fragment;
    sampler.sampler.type = WGPUSamplerBindingType_Filtering;
    entries.push_back(sampler);
  }
  WGPUBindGroupLayoutDescriptor desc = {};
  desc.entryCount = entries.size();
  desc.entries = entries.data();
  return wgpu::BindGroupLayout(wgpuDeviceCreateBindGroupLayout(device, &desc));
}

wgpu::Buffer makeBuffer(WGPUDevice device, uint64_t size, WGPUBufferUsage usage)
{
  return wgpu::Buffer(wgpu::makeBuffer(device, size, usage));
}

// A vertex attribute, by field name: newer webgpu.h (the browser's) puts a
// nextInChain first, so a braced list wouldn't line up.
WGPUVertexAttribute
attribute(WGPUVertexFormat format, uint64_t offset, uint32_t location)
{
  WGPUVertexAttribute a = {};
  a.format = format;
  a.offset = offset;
  a.shaderLocation = location;
  return a;
}

// The two vertex buffers every mesh pipeline reads: per-vertex mesh data and
// per-instance model matrix, tint and UV rectangle.
struct MeshVertexLayouts
{
  std::array<WGPUVertexAttribute, 4> vertexAttributes{
      attribute(WGPUVertexFormat_Float32x3, offsetof(MeshVertex, position), 0),
      attribute(WGPUVertexFormat_Float32x3, offsetof(MeshVertex, normal), 1),
      attribute(WGPUVertexFormat_Float32x3, offsetof(MeshVertex, color), 2),
      attribute(WGPUVertexFormat_Float32x2, offsetof(MeshVertex, uv), 8),
  };
  std::array<WGPUVertexAttribute, 6> instanceAttributes{
      attribute(WGPUVertexFormat_Float32x4,
                offsetof(MeshInstance, model) + 0,
                3),
      attribute(WGPUVertexFormat_Float32x4,
                offsetof(MeshInstance, model) + 16,
                4),
      attribute(WGPUVertexFormat_Float32x4,
                offsetof(MeshInstance, model) + 32,
                5),
      attribute(WGPUVertexFormat_Float32x4,
                offsetof(MeshInstance, model) + 48,
                6),
      attribute(WGPUVertexFormat_Float32x4, offsetof(MeshInstance, tint), 7),
      attribute(WGPUVertexFormat_Float32x4, offsetof(MeshInstance, uvRect), 9),
  };
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
                                   wgpu::Texture& outTexture,
                                   uint32_t samples = 1)
{
  WGPUTextureDescriptor desc = {};
  desc.usage = usage;
  desc.dimension = WGPUTextureDimension_2D;
  desc.size = {static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), 1};
  desc.format = kDepthFormat;
  desc.mipLevelCount = 1;
  desc.sampleCount = samples;
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

WebGpuRenderer::WebGpuRenderer(WebGpuContext& context,
                               uint32_t shadowMapResolution)
    : m_context(context), m_shadowResolution(std::max(shadowMapResolution, 1u))
{
}

WebGpuRenderer::~WebGpuRenderer() = default;

bool WebGpuRenderer::initialize()
{
  // wgpu's OpenGL fallback (for machines without Vulkan, D3D12 or Metal)
  // can't resolve multisampled HDR targets or compare against cube shadow
  // maps: there, edges aren't smoothed and point lights cast no shadows.
  m_sampleCount = m_context.onOpenGL() ? 1 : kSceneSamplesMost;
  m_capabilities.antiAliasing = m_sampleCount > 1;
  m_capabilities.pointLightShadows = !m_context.onOpenGL();
  // Cascades share one atlas, twice the map's size across: only where the
  // device can make one that big (else the one map, as ever).
  m_capabilities.sunShadowCascades =
      m_shadowResolution * 2 <= m_context.maxTextureSize() ? kMaxShadowCascades
                                                           : 1;
  m_frameUniforms =
      makeBuffer(m_context.device(),
                 sizeof(FrameUniforms),
                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  m_cascadeUniforms =
      makeBuffer(m_context.device(),
                 kFaceStride * kMaxShadowCascades,
                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  createShadowMap();

  if (!createTextureResources() || !createPipelines())
  {
    LOG_ERROR("WebGpuRenderer: failed to build render pipelines");
    return false;
  }
  return true;
}

void WebGpuRenderer::setTarget(WGPUCommandEncoder encoder,
                               WGPUTextureView target,
                               glm::ivec2 size)
{
  m_encoder = encoder;
  m_target = target;
  m_viewport = size;
}

MeshHandle WebGpuRenderer::createMesh(const MeshData& mesh)
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
  const Aabb bounds = mesh.bounds();
  gpu.boundsCenter = bounds.center();
  for (const MeshVertex& vertex : mesh.vertices)
    gpu.boundsRadius = std::max(
        gpu.boundsRadius, glm::distance(vertex.position, gpu.boundsCenter));

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
    if (slot >= kMeshSlotMask)
    {
      LOG_ERROR("Too many meshes (" + std::to_string(slot) + ")");
      return {};
    }
    m_meshes.emplace_back(std::move(gpu));
    m_meshGenerations.push_back(0);
  }

  return MeshHandle{(slot + 1) | (m_meshGenerations[slot] << kMeshSlotBits)};
}

void WebGpuRenderer::destroyMesh(MeshHandle mesh)
{
  if (!findMesh(mesh))
    return;

  const uint32_t slot = meshSlotOf(mesh);
  m_meshes[slot].reset();
  // Handles to it no longer match the slot.
  m_meshGenerations[slot] = (m_meshGenerations[slot] + 1) % kMeshGenerations;
  m_freeSlots.push_back(slot);
}

MaterialHandle WebGpuRenderer::createMaterial(const MeshMaterial& material)
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

void WebGpuRenderer::updateMaterial(MaterialHandle handle,
                                    const MeshMaterial& material)
{
  if (!handle.valid() || handle.id > m_materials.size() ||
      !m_materials[handle.id - 1])
    return;
  if (std::optional<GpuMaterial> gpu = makeMaterial(material))
    m_materials[handle.id - 1] = std::move(gpu);
}

void WebGpuRenderer::destroyMaterial(MaterialHandle material)
{
  if (!material.valid() || material.id > m_materials.size() ||
      !m_materials[material.id - 1])
    return;
  m_materials[material.id - 1].reset();
  m_freeMaterialSlots.push_back(material.id - 1);
}

WebGpuRenderer::GpuImage WebGpuRenderer::makeImage(WGPUDevice device,
                                                   glm::ivec2 size,
                                                   WGPUTextureFormat format)
{
  WGPUTextureDescriptor desc = {};
  desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
  desc.dimension = WGPUTextureDimension_2D;
  desc.size = {static_cast<uint32_t>(std::max(size.x, 1)),
               static_cast<uint32_t>(std::max(size.y, 1)),
               1};
  desc.format = format;
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;
  GpuImage image;
  image.texture.reset(wgpuDeviceCreateTexture(device, &desc));
  image.view.reset(wgpuTextureCreateView(image.texture.get(), nullptr));
  image.size = glm::max(size, glm::ivec2(1));
  return image;
}

void WebGpuRenderer::writeImage(WGPUQueue queue,
                                const GpuImage& image,
                                glm::ivec2 origin,
                                glm::ivec2 size,
                                const void* pixels,
                                uint32_t bytesPerPixel)
{
  writeTexture(queue, image.texture.get(), origin, size, pixels, bytesPerPixel);
}

void WebGpuRenderer::writeTexture(WGPUQueue queue,
                                  WGPUTexture texture,
                                  glm::ivec2 origin,
                                  glm::ivec2 size,
                                  const void* pixels,
                                  uint32_t bytesPerPixel)
{
  if (size.x <= 0 || size.y <= 0 || !texture)
    return;
  WGPUTexelCopyTextureInfo destination = {};
  destination.texture = texture;
  destination.origin = {
      static_cast<uint32_t>(origin.x), static_cast<uint32_t>(origin.y), 0};
  destination.aspect = WGPUTextureAspect_All;
  WGPUTexelCopyBufferLayout layout = {};
  layout.bytesPerRow = static_cast<uint32_t>(size.x) * bytesPerPixel;
  layout.rowsPerImage = static_cast<uint32_t>(size.y);
  const WGPUExtent3D extent = {
      static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), 1};
  wgpuQueueWriteTexture(queue,
                        &destination,
                        pixels,
                        std::size_t(size.x) * size.y * bytesPerPixel,
                        &layout,
                        &extent);
}

GrassHandle WebGpuRenderer::createGrass(const GrassGround& ground)
{
  if (ground.empty())
    return {};
  const WGPUDevice device = m_context.device();
  const WGPUQueue queue = m_context.queue();

  GpuGrass grass;
  grass.state.groundGrid = ground.grid();
  const glm::vec2 texel = ground.grid().texelSize();
  grass.state.groundArea =
      static_cast<float>(ground.groundTexels()) * texel.x * texel.y;
  grass.ground =
      makeImage(device, ground.grid().size, WGPUTextureFormat_RGBA32Float);
  writeImage(queue,
             grass.ground,
             {0, 0},
             ground.grid().size,
             ground.texels().data(),
             sizeof(glm::vec4));

  // Unpainted (full, full-length grass, no data) and nothing flattened, until
  // told otherwise.
  const uint8_t full[4] = {255, 255, 0, 0};
  const uint8_t none[4] = {0, 0, 0, 0};
  const uint8_t standing[4] = {128, 128, 0, 255};
  grass.shape = makeImage(device, {1, 1}, WGPUTextureFormat_RGBA8Unorm);
  grass.data = makeImage(device, {1, 1}, WGPUTextureFormat_RGBA8Unorm);
  grass.trample = makeImage(device, {1, 1}, WGPUTextureFormat_RGBA8Unorm);
  writeImage(queue, grass.shape, {0, 0}, {1, 1}, full, 4);
  writeImage(queue, grass.data, {0, 0}, {1, 1}, none, 4);
  writeImage(queue, grass.trample, {0, 0}, {1, 1}, standing, 4);

  grass.growUniforms =
      makeBuffer(device,
                 sizeof(GrassGrowUniforms),
                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  grass.drawUniforms =
      makeBuffer(device,
                 sizeof(GrassUniforms),
                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  grass.draws = makeBuffer(device,
                           12 * sizeof(uint32_t),
                           WGPUBufferUsage_Storage | WGPUBufferUsage_Indirect |
                               WGPUBufferUsage_CopyDst);
  // Room for a few blades until the first frame says how many it needs.
  grass.state.capacity = {64, 64, 64};
  grass.blades =
      makeBuffer(device, 3 * 64 * kGrownBladeBytes, WGPUBufferUsage_Storage);
  if (!grass.ground.texture || !grass.growUniforms || !grass.drawUniforms ||
      !grass.draws || !grass.blades)
    return {};
  rebuildGrassBindings(grass);

  uint32_t slot = 0;
  if (!m_freeGrassSlots.empty())
  {
    slot = m_freeGrassSlots.back();
    m_freeGrassSlots.pop_back();
    m_grass[slot] = std::move(grass);
  }
  else
  {
    slot = static_cast<uint32_t>(m_grass.size());
    m_grass.emplace_back(std::move(grass));
  }
  return GrassHandle{slot + 1};
}

void WebGpuRenderer::rebuildGrassBindings(GpuGrass& grass)
{
  const WGPUDevice device = m_context.device();
  {
    std::array<WGPUBindGroupEntry, 6> entries{};
    entries[0].binding = 0;
    entries[0].buffer = grass.growUniforms.get();
    entries[0].size = sizeof(GrassGrowUniforms);
    entries[1].binding = 1;
    entries[1].textureView = grass.ground.view.get();
    entries[2].binding = 2;
    entries[2].textureView = grass.shape.view.get();
    entries[3].binding = 3;
    entries[3].sampler = m_clampSampler.get();
    entries[4].binding = 4;
    entries[4].buffer = grass.blades.get();
    entries[4].size = WGPU_WHOLE_SIZE;
    entries[5].binding = 5;
    entries[5].buffer = grass.draws.get();
    entries[5].size = WGPU_WHOLE_SIZE;
    WGPUBindGroupDescriptor desc = {};
    desc.layout = m_grassGrowLayout.get();
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    grass.growBindings.reset(wgpuDeviceCreateBindGroup(device, &desc));
  }
  uint64_t first = 0;
  for (std::size_t list = 0; list < grass.drawBindings.size(); ++list)
  {
    std::array<WGPUBindGroupEntry, 6> entries{};
    entries[0].binding = 0;
    entries[0].buffer = grass.blades.get();
    entries[0].offset = first * kGrownBladeBytes;
    entries[0].size = uint64_t(grass.state.capacity[list]) * kGrownBladeBytes;
    entries[1].binding = 1;
    entries[1].buffer = grass.drawUniforms.get();
    entries[1].size = sizeof(GrassUniforms);
    entries[2].binding = 2;
    entries[2].textureView = grass.trample.view.get();
    entries[3].binding = 3;
    entries[3].sampler = m_clampSampler.get();
    entries[4].binding = 4;
    entries[4].textureView = grass.data.view.get();
    entries[5].binding = 5;
    entries[5].textureView = grass.shape.view.get();
    WGPUBindGroupDescriptor desc = {};
    desc.layout = m_grassLayout.get();
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    grass.drawBindings[list].reset(wgpuDeviceCreateBindGroup(device, &desc));
    first += grass.state.capacity[list];
  }
}

void WebGpuRenderer::updateGrassMaps(GrassHandle handle,
                                     const GrassMapsImage& image)
{
  if (!handle.valid() || handle.id > m_grass.size() || !m_grass[handle.id - 1])
    return;
  GpuGrass& grass = *m_grass[handle.id - 1];
  const WGPUDevice device = m_context.device();
  const WGPUQueue queue = m_context.queue();

  if (image.grid.empty())
  {
    // Unpainted: full, full-length grass.
    if (grass.shape.size != glm::ivec2(1))
    {
      grass.shape = makeImage(device, {1, 1}, WGPUTextureFormat_RGBA8Unorm);
      grass.data = makeImage(device, {1, 1}, WGPUTextureFormat_RGBA8Unorm);
      rebuildGrassBindings(grass);
    }
    const uint8_t full[4] = {255, 255, 0, 0};
    const uint8_t none[4] = {0, 0, 0, 0};
    writeImage(queue, grass.shape, {0, 0}, {1, 1}, full, 4);
    writeImage(queue, grass.data, {0, 0}, {1, 1}, none, 4);
    grass.state.mapsGrid = {};
    return;
  }

  const glm::ivec2 from =
      glm::clamp(image.from, glm::ivec2(0), image.grid.size);
  const glm::ivec2 to = glm::clamp(image.to, from, image.grid.size);
  const std::size_t bytes = std::size_t(to.x - from.x) * (to.y - from.y) * 4;
  if (image.shape.size() < bytes || image.data.size() < bytes)
    return;
  if (grass.state.mapsGrid.size != image.grid.size ||
      grass.shape.size != image.grid.size)
  {
    grass.shape =
        makeImage(device, image.grid.size, WGPUTextureFormat_RGBA8Unorm);
    grass.data =
        makeImage(device, image.grid.size, WGPUTextureFormat_RGBA8Unorm);
    rebuildGrassBindings(grass);
  }
  grass.state.mapsGrid = image.grid;
  writeImage(queue, grass.shape, from, to - from, image.shape.data(), 4);
  writeImage(queue, grass.data, from, to - from, image.data.data(), 4);
}

void WebGpuRenderer::updateGrassTrample(GrassHandle handle,
                                        const GrassTrampleImage& image)
{
  if (!handle.valid() || handle.id > m_grass.size() || !m_grass[handle.id - 1])
    return;
  if (image.size.x <= 0 || image.size.y <= 0 ||
      image.pixels.size() < std::size_t(image.size.x) * image.size.y * 4)
    return;
  GpuGrass& grass = *m_grass[handle.id - 1];

  // A texture of the map's size (made again when that changes).
  if (grass.trample.size != image.size)
  {
    grass.trample =
        makeImage(m_context.device(), image.size, WGPUTextureFormat_RGBA8Unorm);
    rebuildGrassBindings(grass);
  }
  writeImage(m_context.queue(),
             grass.trample,
             {0, 0},
             image.size,
             image.pixels.data(),
             4);
  const glm::vec2 size = glm::max(image.max - image.min, glm::vec2(1e-4f));
  grass.state.trampleBounds = glm::vec4(image.min, 1.0f / size);
}

void WebGpuRenderer::destroyGrass(GrassHandle grass)
{
  if (!grass.valid() || grass.id > m_grass.size() || !m_grass[grass.id - 1])
    return;
  m_grass[grass.id - 1].reset();
  m_freeGrassSlots.push_back(grass.id - 1);
}

void WebGpuRenderer::drawGrass(const GrassBatch& batch)
{
  if (batch.grass.valid() && batch.grass.id <= m_grass.size() &&
      m_grass[batch.grass.id - 1])
    m_plan.addGrass(batch);
}

WGPURenderPipeline WebGpuRenderer::grassPipeline(const Surface& surface,
                                                 bool ground)
{
  wgpu::RenderPipeline& built = ground ? surface.grassGround : surface.grass;
  bool& broken = ground ? surface.grassGroundBroken : surface.grassBroken;
  if (built || broken)
    return built.get();
  const WGPUDevice device = m_context.device();
  ValidationScope scope(device);

  const wgpu::ShaderModule module(wgpu::makeShader(
      device,
      shaders::meshPass(surface.wgsl, shaders::litPass(shaders::meshGrassWgsl))
          .c_str()));
  const std::array<WGPUBindGroupLayout, 3> groups = {
      m_litLayout.get(), surface.materialLayout.get(), m_grassLayout.get()};
  WGPUPipelineLayoutDescriptor layoutDesc = {};
  layoutDesc.bindGroupLayoutCount = groups.size();
  layoutDesc.bindGroupLayouts = groups.data();
  const wgpu::PipelineLayout layout(
      wgpuDeviceCreatePipelineLayout(device, &layoutDesc));

  // Blades: as the opaque lit pipeline, but they come from storage (no
  // vertex buffers), as strips, seen from both sides. The ground: its mesh
  // again, blended over itself (pulled a little toward the camera, so it
  // wins where they're level).
  WGPUDepthStencilState depth =
      ground ? depthState(false, WGPUCompareFunction_LessEqual)
             : depthState(true, WGPUCompareFunction_Less);
  if (ground)
  {
    depth.depthBias = -8;
    depth.depthBiasSlopeScale = -1.0f;
  }
  WGPUBlendState blending = {};
  blending.color = {WGPUBlendOperation_Add,
                    WGPUBlendFactor_SrcAlpha,
                    WGPUBlendFactor_OneMinusSrcAlpha};
  blending.alpha = {
      WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One};
  std::array<WGPUColorTargetState, 2> colors{};
  for (WGPUColorTargetState& color : colors)
  {
    color.format = kSceneFormat;
    color.writeMask = WGPUColorWriteMask_All;
    color.blend = ground ? &blending : nullptr;
  }
  WGPUFragmentState fragment = {};
  fragment.module = module.get();
  fragment.entryPoint = wgpu::sv(ground ? "fs_ground" : "fs_main");
  fragment.targetCount = colors.size();
  fragment.targets = colors.data();

  const MeshVertexLayouts layouts;
  WGPURenderPipelineDescriptor desc = {};
  desc.layout = layout.get();
  desc.vertex.module = module.get();
  desc.vertex.entryPoint = wgpu::sv(ground ? "vs_ground" : "vs_main");
  if (ground)
  {
    desc.vertex.bufferCount = 1; // the mesh's vertices, no instances
    desc.vertex.buffers = layouts.buffers.data();
  }
  desc.primitive.topology = ground ? WGPUPrimitiveTopology_TriangleList
                                   : WGPUPrimitiveTopology_TriangleStrip;
  desc.primitive.frontFace = WGPUFrontFace_CCW;
  desc.primitive.cullMode = ground ? WGPUCullMode_Back : WGPUCullMode_None;
  desc.depthStencil = &depth;
  desc.multisample.count = m_sampleCount;
  desc.multisample.mask = 0xFFFFFFFFu;
  desc.fragment = &fragment;
  built.reset(wgpuDeviceCreateRenderPipeline(device, &desc));

  if (const std::optional<std::string> error = scope.end())
  {
    LOG_ERROR("A material's shader doesn't build for grass: " + *error);
    built.reset();
  }
  broken = !built;
  return built.get();
}

void WebGpuRenderer::growGrass()
{
  for (std::optional<GpuGrass>& grass : m_grass)
    if (grass)
      grass->grown = grass->placed = false;
  if (m_plan.grassDraws().empty() || !m_grassGrowPipeline)
    return;
  const WGPUQueue queue = m_context.queue();

  WGPUComputePassDescriptor passDesc = {};
  const WGPUComputePassEncoder pass =
      wgpuCommandEncoderBeginComputePass(m_encoder, &passDesc);
  wgpuComputePassEncoderSetPipeline(pass, m_grassGrowPipeline.get());
  for (const GrassBatch& batch : m_plan.grassDraws())
  {
    GpuGrass& grass = *m_grass[batch.grass.id - 1];
    if (grass.placed)
      continue;
    const std::optional<GrassPlan> plan =
        planGrass(batch, grass.state, m_plan.frame());
    if (!plan)
      continue;

    // How it's drawn (its ground, even with no blades in reach).
    wgpuQueueWriteBuffer(
        queue, grass.drawUniforms.get(), 0, &plan->draw, sizeof(plan->draw));
    grass.placed = true;
    if (!plan->grows)
      continue;

    if (plan->capacity != grass.state.capacity)
    {
      grass.state.capacity = plan->capacity;
      uint64_t blades = 0;
      for (const uint32_t capacity : plan->capacity)
        blades += capacity;
      grass.blades = makeBuffer(m_context.device(),
                                blades * kGrownBladeBytes,
                                WGPUBufferUsage_Storage);
      if (!grass.blades)
        continue;
      rebuildGrassBindings(grass);
    }

    // Each list's draw: its blades' strip (2 vertices a segment, and the tip),
    // as many as grow.
    const std::array<uint32_t, 12> draws = {
        11, 0, 0, 0, 5, 0, 0, 0, 3, 0, 0, 0};
    wgpuQueueWriteBuffer(
        queue, grass.draws.get(), 0, draws.data(), sizeof(draws));
    wgpuQueueWriteBuffer(
        queue, grass.growUniforms.get(), 0, &plan->grow, sizeof(plan->grow));

    wgpuComputePassEncoderSetBindGroup(
        pass, 0, grass.growBindings.get(), 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, plan->workgroups.x, plan->workgroups.y, plan->workgroups.z);
    grass.grown = true;
  }
  wgpuComputePassEncoderEnd(pass);
  wgpuComputePassEncoderRelease(pass);
}

void WebGpuRenderer::encodeGrass(WGPURenderPassEncoder pass)
{
  for (const GrassBatch& batch : m_plan.grassDraws())
  {
    GpuGrass& grass = *m_grass[batch.grass.id - 1];
    if (!grass.placed)
      continue;
    const bool grown = grass.grown;
    grass.placed = grass.grown = false; // drawn once, though queued twice
    const uint32_t id = batch.material.id;
    const GpuMaterial* material =
        id > 0 && id <= m_materials.size() && m_materials[id - 1]
            ? &*m_materials[id - 1]
            : nullptr;
    if (!material || !material->surface)
      continue;
    m_boundPipeline = nullptr; // the mesh pipelines need setting again after

    // The ground first, tinted where grass covers it.
    const GpuMesh* mesh = findMesh(batch.mesh);
    if (const WGPURenderPipeline ground =
            mesh ? grassPipeline(*material->surface, true) : nullptr)
    {
      wgpuRenderPassEncoderSetPipeline(pass, ground);
      wgpuRenderPassEncoderSetBindGroup(
          pass, 1, material->bindings.get(), 0, nullptr);
      wgpuRenderPassEncoderSetBindGroup(
          pass, 2, grass.drawBindings[0].get(), 0, nullptr);
      wgpuRenderPassEncoderSetVertexBuffer(
          pass, 0, mesh->vertices.get(), 0, WGPU_WHOLE_SIZE);
      wgpuRenderPassEncoderSetIndexBuffer(pass,
                                          mesh->indices.get(),
                                          WGPUIndexFormat_Uint32,
                                          0,
                                          WGPU_WHOLE_SIZE);
      wgpuRenderPassEncoderDrawIndexed(pass, mesh->indexCount, 1, 0, 0, 0);
    }

    const WGPURenderPipeline blades =
        grown ? grassPipeline(*material->surface, false) : nullptr;
    if (!blades)
      continue;
    wgpuRenderPassEncoderSetPipeline(pass, blades);
    wgpuRenderPassEncoderSetBindGroup(
        pass, 1, material->bindings.get(), 0, nullptr);
    for (std::size_t list = 0; list < grass.drawBindings.size(); ++list)
    {
      wgpuRenderPassEncoderSetBindGroup(
          pass, 2, grass.drawBindings[list].get(), 0, nullptr);
      wgpuRenderPassEncoderDrawIndirect(
          pass, grass.draws.get(), list * 4 * sizeof(uint32_t));
    }
  }
}

void WebGpuRenderer::beginFrame(const FrameParams& frame)
{
  if (m_capabilities.pointLightShadows)
  {
    m_plan.begin(frame);
    return;
  }
  FrameParams unshadowed = frame;
  unshadowed.shadows.pointLights = 0;
  m_plan.begin(unshadowed);
}

void WebGpuRenderer::drawMesh(const MeshBatch& batch)
{
  const GpuMesh* mesh = m_plan.open() ? findMesh(batch.mesh) : nullptr;
  if (!mesh)
    return;

  // Whether any section draws see-through with the batch's materials.
  bool hasBlend = false;
  for (std::size_t i = 0; i < mesh->sections.size(); ++i)
  {
    const uint32_t id = i < batch.materials.size() ? batch.materials[i].id : 0;
    const GpuMaterial& material =
        id > 0 && id <= m_materials.size() && m_materials[id - 1]
            ? *m_materials[id - 1]
            : mesh->sections[i].material;
    hasBlend = hasBlend || isSeeThrough(material.alphaMode);
  }
  m_plan.addMesh(batch, meshSlotOf(batch.mesh), plannedMesh(*mesh), hasBlend);
}

void WebGpuRenderer::endFrame()
{
  if (!m_plan.open())
    return;

  const uint32_t pointResolution = m_plan.frame().shadows.pointResolution;
  m_plan.finish(m_viewport,
                m_shadowResolution,
                pointResolution,
                [this](uint32_t slot) -> const PlannedMesh*
                {
                  const std::optional<GpuMesh>& mesh = m_meshes[slot];
                  if (!mesh)
                    return nullptr;
                  m_plannedMesh = plannedMesh(*mesh);
                  return &m_plannedMesh;
                });

  if (!m_encoder || !m_target || m_viewport.x <= 0 || m_viewport.y <= 0)
    return;

  ensureTargets();
  if (!m_plan.shadowedLights().empty())
    ensurePointShadowMaps(
        static_cast<uint32_t>(m_plan.shadowedLights().size()), pointResolution);
  if (m_plan.frame().shadows.enabled)
    ensureShadowAtlas(m_plan.shadowCascades());
  encodeSky();
  m_occluded = prepareOcclusion();
  uploadFrame();
  uploadLights();

  if (m_plan.frame().shadows.enabled)
    encodeShadowPass();
  encodePointShadows();
  growGrass();
  if (m_occluded)
    encodeOcclusion();
  encodeLitPass();
  m_focused = encodeDepthOfField();
  if (m_plan.frame().bloom.enabled && !m_bloom.empty())
    encodeBloom();
  encodeComposite();
  encodeUi();
}

void WebGpuRenderer::ensureShadowAtlas(uint32_t cascades)
{
  const glm::uvec2 size = shadowAtlasSize(
      std::min(cascades, m_capabilities.sunShadowCascades), m_shadowResolution);
  if (m_shadowMap && size == m_shadowMapSize)
    return;
  m_shadowView = makeDepthTexture(
      m_context.device(),
      glm::ivec2(size),
      WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
      m_shadowMap);
  m_shadowMapSize = size;
  rebuildLitBindings();
}

void WebGpuRenderer::createShadowMap()
{
  ensureShadowAtlas(1);

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

bool WebGpuRenderer::createTextureResources()
{
  const WGPUDevice device = m_context.device();

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

  // What the lit pass binds for ambient occlusion while there's none (never
  // read then).
  {
    WGPUTextureDescriptor desc = {};
    desc.usage = WGPUTextureUsage_TextureBinding;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = {1, 1, 1};
    desc.format = kSceneFormat;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    m_noOcclusion.reset(wgpuDeviceCreateTexture(device, &desc));
    if (m_noOcclusion)
      m_noOcclusionView.reset(
          wgpuTextureCreateView(m_noOcclusion.get(), nullptr));
  }
  return m_linearSampler && m_nearestSampler && m_white && m_noOcclusionView;
}

std::shared_ptr<WebGpuRenderer::GpuTexture>
WebGpuRenderer::uploadTexture(const std::shared_ptr<const TextureData>& texture,
                              bool mipmaps)
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

  // Bigger than the device can make (a huge image, a modest GPU): halved
  // until it fits.
  uint32_t width = texture->width, height = texture->height;
  const std::vector<uint8_t>* texels = &texture->rgba;
  std::vector<uint8_t> shrunk;
  const uint32_t most = std::max(m_context.maxTextureSize(), 1u);
  if (width > most || height > most)
    LOG_INFO("A " + std::to_string(width) + "x" + std::to_string(height) +
             " texture is bigger than this GPU allows (" +
             std::to_string(most) + "); it's drawn at a smaller size");
  while (width > most || height > most)
  {
    uint32_t nextWidth = 0, nextHeight = 0;
    shrunk = halve(*texels, width, height, nextWidth, nextHeight);
    texels = &shrunk;
    width = nextWidth;
    height = nextHeight;
  }

  const WGPUDevice device = m_context.device();
  const uint32_t levels = mipmaps ? mipLevels(width, height) : 1;
  WGPUTextureDescriptor desc = {};
  desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
  desc.dimension = WGPUTextureDimension_2D;
  desc.size = {width, height, 1};
  // Colour images are sRGB, so sampling gives linear colour; data stays as is.
  desc.format = texture->linear ? WGPUTextureFormat_RGBA8Unorm
                                : WGPUTextureFormat_RGBA8UnormSrgb;
  desc.mipLevelCount = levels;
  desc.sampleCount = 1;

  auto gpu = std::make_shared<GpuTexture>();
  gpu->nearest = texture->nearest;
  gpu->levels = levels;
  gpu->size = {width, height};
  gpu->texture.reset(wgpuDeviceCreateTexture(device, &desc));
  if (!gpu->texture)
    return nullptr;

  writeLevel(m_context.queue(), gpu->texture.get(), 0, *texels, width, height);
  std::vector<uint8_t> level;
  const std::vector<uint8_t>* previous = texels;
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

std::shared_ptr<WebGpuRenderer::GpuTexture>
WebGpuRenderer::solidTexture(const glm::vec4& value)
{
  std::array<uint8_t, 4> rgba{};
  for (int i = 0; i < 4; ++i)
    rgba[i] = static_cast<uint8_t>(
        std::lround(std::clamp(value[i], 0.0f, 1.0f) * 255.0f));
  const uint32_t key = uint32_t(rgba[0]) | uint32_t(rgba[1]) << 8 |
                       uint32_t(rgba[2]) << 16 | uint32_t(rgba[3]) << 24;
  if (const auto it = m_solidTextures.find(key); it != m_solidTextures.end())
    return it->second;

  auto texture = std::make_shared<TextureData>();
  texture->width = texture->height = 1;
  texture->rgba.assign(rgba.begin(), rgba.end());
  texture->linear = true;
  std::shared_ptr<GpuTexture> gpu = uploadTexture(texture);
  if (!gpu)
    return m_white;
  m_solidTextures.emplace(key, gpu);
  return gpu;
}

std::optional<WebGpuRenderer::GpuMaterial>
WebGpuRenderer::makeMaterial(const MeshMaterial& material)
{
  if (material.terrain)
  {
    if (std::optional<GpuMaterial> gpu = makeTerrainMaterial(material))
      return gpu;
    return makeStandardMaterial(brokenMaterial());
  }
  if (!material.shader)
    return makeStandardMaterial(material);
  if (std::optional<GpuMaterial> gpu = makeGraphMaterial(material))
    return gpu;
  // A shader that doesn't build draws magenta, so it's noticed.
  return makeStandardMaterial(brokenMaterial());
}

std::optional<WebGpuRenderer::GpuMaterial>
WebGpuRenderer::makeStandardMaterial(const MeshMaterial& material)
{
  const WGPUDevice device = m_context.device();
  GpuMaterial gpu;
  gpu.alphaMode = material.alphaMode;
  gpu.surface = m_standard;
  std::shared_ptr<GpuTexture> base = uploadTexture(material.baseTexture);
  std::shared_ptr<GpuTexture> metallicRoughness =
      uploadTexture(material.metallicRoughnessTexture);
  std::shared_ptr<GpuTexture> normal = uploadTexture(material.normalTexture);
  if (!base)
    base = m_white;
  if (!metallicRoughness)
    metallicRoughness = m_white;
  if (!normal)
    normal = solidTexture(materialGraph::kFlatNormal);

  const MaterialUniforms uniforms = standardMaterialUniforms(material);
  gpu.uniforms = makeBuffer(device,
                            sizeof(uniforms),
                            WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  if (!gpu.uniforms)
    return std::nullopt;
  wgpuQueueWriteBuffer(
      m_context.queue(), gpu.uniforms.get(), 0, &uniforms, sizeof(uniforms));

  std::array<WGPUBindGroupEntry, 5> entries{};
  entries[0].binding = 0;
  entries[0].textureView = base->view.get();
  entries[1].binding = 1;
  entries[1].sampler =
      base->nearest ? m_nearestSampler.get() : m_linearSampler.get();
  entries[2].binding = 2;
  entries[2].textureView = metallicRoughness->view.get();
  entries[3].binding = 3;
  entries[3].buffer = gpu.uniforms.get();
  entries[3].size = sizeof(MaterialUniforms);
  entries[4].binding = 4;
  entries[4].textureView = normal->view.get();

  WGPUBindGroupDescriptor desc = {};
  desc.layout = gpu.surface->materialLayout.get();
  desc.entryCount = entries.size();
  desc.entries = entries.data();
  gpu.bindings.reset(wgpuDeviceCreateBindGroup(device, &desc));
  if (!gpu.bindings)
    return std::nullopt;
  gpu.textures = {
      std::move(base), std::move(metallicRoughness), std::move(normal)};
  return gpu;
}

std::optional<WebGpuRenderer::GpuMaterial>
WebGpuRenderer::makeGraphMaterial(const MeshMaterial& material)
{
  const materialGraph::MaterialShader& shader = *material.shader;
  const WGPUDevice device = m_context.device();
  GpuMaterial gpu;
  gpu.alphaMode = material.alphaMode;
  gpu.surface = graphSurface(shader, material.alphaMode);
  if (!gpu.surface)
    return std::nullopt;

  const std::vector<glm::vec4> parameters = graphMaterialParameters(material);
  const uint64_t bytes = parameters.size() * sizeof(glm::vec4);
  gpu.uniforms = makeBuffer(
      device, bytes, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  if (!gpu.uniforms)
    return std::nullopt;
  wgpuQueueWriteBuffer(
      m_context.queue(), gpu.uniforms.get(), 0, parameters.data(), bytes);

  std::vector<WGPUBindGroupEntry> entries(1 + 2 * shader.textureCount());
  entries[0].binding = 0;
  entries[0].buffer = gpu.uniforms.get();
  entries[0].size = bytes;
  for (std::size_t i = 0; i < shader.textureCount(); ++i)
  {
    std::shared_ptr<GpuTexture> texture =
        i < material.shaderTextures.size()
            ? uploadTexture(material.shaderTextures[i])
            : nullptr;
    if (!texture)
      texture = solidTexture(shader.missingTexture(i));
    entries[1 + 2 * i].binding = static_cast<uint32_t>(1 + 2 * i);
    entries[1 + 2 * i].textureView = texture->view.get();
    entries[2 + 2 * i].binding = static_cast<uint32_t>(2 + 2 * i);
    entries[2 + 2 * i].sampler =
        texture->nearest ? m_nearestSampler.get() : m_linearSampler.get();
    gpu.textures.push_back(std::move(texture));
  }

  WGPUBindGroupDescriptor desc = {};
  desc.layout = gpu.surface->materialLayout.get();
  desc.entryCount = entries.size();
  desc.entries = entries.data();
  gpu.bindings.reset(wgpuDeviceCreateBindGroup(device, &desc));
  if (!gpu.bindings)
    return std::nullopt;
  return gpu;
}

namespace
{

// An RGBA8 image resized (bilinearly) to `width` x `height`.
std::vector<uint8_t>
resized(const TextureData& image, uint32_t width, uint32_t height)
{
  std::vector<uint8_t> out(std::size_t(width) * height * 4);
  if (image.width == width && image.height == height)
  {
    std::copy_n(image.rgba.begin(), out.size(), out.begin());
    return out;
  }
  const float sx = static_cast<float>(image.width) / static_cast<float>(width);
  const float sy =
      static_cast<float>(image.height) / static_cast<float>(height);
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x)
    {
      // Repeating at the edges, as the textures tile.
      const float fx = (static_cast<float>(x) + 0.5f) * sx - 0.5f;
      const float fy = (static_cast<float>(y) + 0.5f) * sy - 0.5f;
      const auto x0 = static_cast<int>(std::floor(fx));
      const auto y0 = static_cast<int>(std::floor(fy));
      const float tx = fx - static_cast<float>(x0);
      const float ty = fy - static_cast<float>(y0);
      const auto wrap = [](int v, uint32_t n)
      {
        const int m = static_cast<int>(n);
        return static_cast<uint32_t>(((v % m) + m) % m);
      };
      const auto at = [&](int px, int py, int c)
      {
        return static_cast<float>(
            image.rgba[(std::size_t(wrap(py, image.height)) * image.width +
                        wrap(px, image.width)) *
                           4 +
                       c]);
      };
      for (int c = 0; c < 4; ++c)
      {
        const float top = at(x0, y0, c) * (1.0f - tx) + at(x0 + 1, y0, c) * tx;
        const float bottom =
            at(x0, y0 + 1, c) * (1.0f - tx) + at(x0 + 1, y0 + 1, c) * tx;
        out[(std::size_t(y) * width + x) * 4 + c] =
            static_cast<uint8_t>(std::lround(
                std::clamp(top * (1.0f - ty) + bottom * ty, 0.0f, 255.0f)));
      }
    }
  return out;
}

bool usableImage(const std::shared_ptr<const TextureData>& image)
{
  return image && !image->empty() &&
         image->rgba.size() >= std::size_t(image->width) * image->height * 4;
}

} // namespace

std::shared_ptr<WebGpuRenderer::GpuTexture>
WebGpuRenderer::terrainLayers(const TerrainMaterial& terrain, bool detail)
{
  const std::size_t count = std::clamp<std::size_t>(
      terrain.layers.size(), 1, std::size_t{kMaxTerrainLayers});
  // Each layer's images: its colour; or its normal map and roughness.
  std::vector<std::shared_ptr<const TextureData>> sources;
  for (std::size_t i = 0; i < count; ++i)
  {
    const TerrainLayer layer =
        i < terrain.layers.size() ? terrain.layers[i] : TerrainLayer{};
    if (detail)
    {
      sources.push_back(layer.normalTexture);
      sources.push_back(layer.roughnessTexture);
    }
    else
      sources.push_back(layer.baseTexture);
  }
  std::string key = detail ? "detail" : "color";
  for (const auto& source : sources)
    key += ":" + std::to_string(reinterpret_cast<std::uintptr_t>(source.get()));
  if (const auto it = m_layerArrays.find(key); it != m_layerArrays.end())
  {
    bool same = it->second.sources.size() == sources.size();
    for (std::size_t i = 0; same && i < sources.size(); ++i)
      same = it->second.sources[i].lock() == sources[i];
    if (same)
      if (std::shared_ptr<GpuTexture> live = it->second.gpu.lock())
        return live;
    m_layerArrays.erase(it);
  }

  // Every layer the size of the largest (within what the device allows).
  uint32_t width = 4, height = 4;
  for (const auto& source : sources)
    if (usableImage(source))
    {
      width = std::max(width, source->width);
      height = std::max(height, source->height);
    }
  const uint32_t most = std::min(m_context.maxTextureSize(), 2048u);
  while (width > most || height > most)
  {
    width = std::max(width / 2, 1u);
    height = std::max(height / 2, 1u);
  }
  // At least two layers: wgpu's OpenGL backend makes a one-layer array a
  // plain 2D texture, which an array binding can't take.
  const auto layers = static_cast<uint32_t>(std::max<std::size_t>(count, 2));
  const uint32_t levels = mipLevels(width, height);

  const WGPUDevice device = m_context.device();
  WGPUTextureDescriptor desc = {};
  desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
  desc.dimension = WGPUTextureDimension_2D;
  desc.size = {width, height, layers};
  desc.format =
      detail ? WGPUTextureFormat_RGBA8Unorm : WGPUTextureFormat_RGBA8UnormSrgb;
  desc.mipLevelCount = levels;
  desc.sampleCount = 1;
  auto gpu = std::make_shared<GpuTexture>();
  gpu->levels = levels;
  gpu->size = {width, height};
  gpu->texture.reset(wgpuDeviceCreateTexture(device, &desc));
  if (!gpu->texture)
    return nullptr;

  for (uint32_t layer = 0; layer < layers; ++layer)
  {
    const std::size_t i = std::min<std::size_t>(layer, count - 1);
    std::vector<uint8_t> pixels(std::size_t(width) * height * 4, 255);
    if (detail)
    {
      // Normal xy (flat without a map), roughness (green of its map; white
      // without one), opaque.
      const std::shared_ptr<const TextureData>& normal = sources[2 * i];
      const std::shared_ptr<const TextureData>& rough = sources[2 * i + 1];
      const std::vector<uint8_t> normals = usableImage(normal)
                                               ? resized(*normal, width, height)
                                               : std::vector<uint8_t>();
      const std::vector<uint8_t> roughness =
          usableImage(rough) ? resized(*rough, width, height)
                             : std::vector<uint8_t>();
      for (std::size_t p = 0; p < std::size_t(width) * height; ++p)
      {
        pixels[p * 4 + 0] = normals.empty() ? 128 : normals[p * 4 + 0];
        pixels[p * 4 + 1] = normals.empty() ? 128 : normals[p * 4 + 1];
        pixels[p * 4 + 2] = roughness.empty() ? 255 : roughness[p * 4 + 1];
        pixels[p * 4 + 3] = 255;
      }
    }
    else if (usableImage(sources[i]))
      pixels = resized(*sources[i], width, height);

    uint32_t levelWidth = width, levelHeight = height;
    std::vector<uint8_t> level = std::move(pixels);
    for (uint32_t mip = 0; mip < levels; ++mip)
    {
      if (mip > 0)
      {
        uint32_t nextWidth = 0, nextHeight = 0;
        level = halve(level, levelWidth, levelHeight, nextWidth, nextHeight);
        levelWidth = nextWidth;
        levelHeight = nextHeight;
      }
      WGPUTexelCopyTextureInfo destination = {};
      destination.texture = gpu->texture.get();
      destination.mipLevel = mip;
      destination.origin = {0, 0, layer};
      destination.aspect = WGPUTextureAspect_All;
      WGPUTexelCopyBufferLayout layout = {};
      layout.bytesPerRow = levelWidth * 4;
      layout.rowsPerImage = levelHeight;
      const WGPUExtent3D extent = {levelWidth, levelHeight, 1};
      wgpuQueueWriteTexture(m_context.queue(),
                            &destination,
                            level.data(),
                            level.size(),
                            &layout,
                            &extent);
    }
  }

  WGPUTextureViewDescriptor view = {};
  view.format = desc.format;
  view.dimension = WGPUTextureViewDimension_2DArray;
  view.baseMipLevel = 0;
  view.mipLevelCount = levels;
  view.baseArrayLayer = 0;
  view.arrayLayerCount = layers;
  view.aspect = WGPUTextureAspect_All;
  gpu->view.reset(wgpuTextureCreateView(gpu->texture.get(), &view));
  if (!gpu->view)
    return nullptr;

  std::erase_if(m_layerArrays,
                [](const auto& entry) { return entry.second.gpu.expired(); });
  CachedLayers& cached = m_layerArrays[key];
  cached.sources.assign(sources.begin(), sources.end());
  cached.gpu = gpu;
  return gpu;
}

std::optional<WebGpuRenderer::GpuMaterial>
WebGpuRenderer::makeTerrainMaterial(const MeshMaterial& material)
{
  const WGPUDevice device = m_context.device();
  // Its surface, the first time: group 1 its parameters, the splat map and
  // its sampler, the layers' arrays and theirs.
  if (!m_terrain && !m_terrainBroken)
  {
    std::array<WGPUBindGroupLayoutEntry, 7> entries{};
    entries[0] = wgpu::storageEntry(
        0, WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform);
    for (const uint32_t binding : {1u, 3u, 4u, 6u})
    {
      entries[binding].binding = binding;
      entries[binding].visibility = WGPUShaderStage_Fragment;
      entries[binding].texture.sampleType = WGPUTextureSampleType_Float;
      entries[binding].texture.viewDimension =
          binding == 1 || binding == 6 ? WGPUTextureViewDimension_2D
                                       : WGPUTextureViewDimension_2DArray;
    }
    for (const uint32_t binding : {2u, 5u})
    {
      entries[binding].binding = binding;
      entries[binding].visibility = WGPUShaderStage_Fragment;
      entries[binding].sampler.type = WGPUSamplerBindingType_Filtering;
    }
    WGPUBindGroupLayoutDescriptor layout = {};
    layout.entryCount = entries.size();
    layout.entries = entries.data();
    m_terrain =
        buildSurface(std::string(shaders::meshTerrainSurfaceWgsl),
                     wgpu::BindGroupLayout(
                         wgpuDeviceCreateBindGroupLayout(device, &layout)));
    if (!m_terrain)
    {
      LOG_ERROR("Terrain materials don't build: they draw magenta");
      m_terrainBroken = true;
    }
  }
  if (!m_terrain)
    return std::nullopt;

  const TerrainMaterial& terrain = *material.terrain;
  GpuMaterial gpu;
  gpu.alphaMode = AlphaMode::Opaque;
  gpu.surface = m_terrain;
  // The splat map without mipmaps, so painting it can update it in place.
  std::shared_ptr<GpuTexture> splat = uploadTexture(terrain.splat, false);
  if (!splat)
    splat = solidTexture(glm::vec4(0.0f));
  std::shared_ptr<GpuTexture> tint = uploadTexture(terrain.tint);
  if (!tint)
    tint = m_white;
  std::shared_ptr<GpuTexture> colors = terrainLayers(terrain, false);
  std::shared_ptr<GpuTexture> details = terrainLayers(terrain, true);
  if (!colors || !details)
    return std::nullopt;

  const TerrainUniforms uniforms = terrainMaterialUniforms(terrain);
  gpu.uniforms = makeBuffer(device,
                            sizeof(uniforms),
                            WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  if (!gpu.uniforms)
    return std::nullopt;
  wgpuQueueWriteBuffer(
      m_context.queue(), gpu.uniforms.get(), 0, &uniforms, sizeof(uniforms));

  std::array<WGPUBindGroupEntry, 7> entries{};
  entries[0].binding = 0;
  entries[0].buffer = gpu.uniforms.get();
  entries[0].size = sizeof(TerrainUniforms);
  entries[6].binding = 6;
  entries[6].textureView = tint->view.get();
  entries[1].binding = 1;
  entries[1].textureView = splat->view.get();
  entries[2].binding = 2;
  entries[2].sampler = m_clampSampler.get();
  entries[3].binding = 3;
  entries[3].textureView = colors->view.get();
  entries[4].binding = 4;
  entries[4].textureView = details->view.get();
  entries[5].binding = 5;
  entries[5].sampler = m_linearSampler.get();
  WGPUBindGroupDescriptor desc = {};
  desc.layout = gpu.surface->materialLayout.get();
  desc.entryCount = entries.size();
  desc.entries = entries.data();
  gpu.bindings.reset(wgpuDeviceCreateBindGroup(device, &desc));
  if (!gpu.bindings)
    return std::nullopt;
  gpu.textures = {
      std::move(splat), std::move(colors), std::move(details), std::move(tint)};
  return gpu;
}

void WebGpuRenderer::updateTexture(
    const std::shared_ptr<const TextureData>& texture,
    glm::ivec2 from,
    glm::ivec2 to)
{
  if (!usableImage(texture))
    return;
  const auto it = m_textures.find(texture.get());
  if (it == m_textures.end() || it->second.source.lock() != texture)
    return;
  const std::shared_ptr<GpuTexture> gpu = it->second.gpu.lock();
  // Only a texture with one level, at its own size (a splat map).
  if (!gpu || gpu->levels != 1 || gpu->size.x != texture->width ||
      gpu->size.y != texture->height)
    return;
  from = glm::clamp(from, glm::ivec2(0), glm::ivec2(gpu->size));
  to = glm::clamp(to, glm::ivec2(0), glm::ivec2(gpu->size));
  if (to.x <= from.x || to.y <= from.y)
    return;
  const glm::ivec2 size = to - from;
  std::vector<uint8_t> rows(std::size_t(size.x) * size.y * 4);
  for (int y = 0; y < size.y; ++y)
    std::copy_n(
        texture->rgba.begin() +
            static_cast<std::ptrdiff_t>(
                (std::size_t(from.y + y) * texture->width + from.x) * 4),
        std::size_t(size.x) * 4,
        rows.begin() +
            static_cast<std::ptrdiff_t>(std::size_t(y) * size.x * 4));
  writeTexture(
      m_context.queue(), gpu->texture.get(), from, size, rows.data(), 4);
}

const WebGpuRenderer::GpuMaterial&
WebGpuRenderer::materialFor(const GpuMesh& mesh,
                            std::size_t index,
                            const Draw& draw) const
{
  if (index < draw.materialCount)
  {
    const uint32_t id = m_plan.drawMaterials()[draw.firstMaterial + index];
    if (id > 0 && id <= m_materials.size() && m_materials[id - 1])
      return *m_materials[id - 1];
  }
  return mesh.sections[index].material;
}

bool WebGpuRenderer::createPipelines()
{
  const WGPUDevice device = m_context.device();

  // Bind group layouts: the shadow pass reads only the frame uniforms; the
  // lit pass also samples the shadow map.
  const WGPUBindGroupLayoutEntry uniformEntry =
      wgpu::storageEntry(0,
                         WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
                         WGPUBufferBindingType_Uniform);

  {
    // And the map (cascade) being drawn's view-projection, at a dynamic
    // offset.
    std::array<WGPUBindGroupLayoutEntry, 2> entries{};
    entries[0] = uniformEntry;
    entries[1] = wgpu::storageEntry(
        1, WGPUShaderStage_Vertex, WGPUBufferBindingType_Uniform);
    entries[1].buffer.hasDynamicOffset = true;
    entries[1].buffer.minBindingSize = sizeof(glm::mat4);
    WGPUBindGroupLayoutDescriptor desc = {};
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    m_shadowLayout.reset(wgpuDeviceCreateBindGroupLayout(device, &desc));
  }
  {
    // Then the point lights, each cluster's range of light indices, and the
    // indices.
    // And the point lights' cube shadow maps.
    // And a physical sky's: its reflections, their sampler, the sky, and the
    // BRDF table.
    // And ambient occlusion's map (read texel by texel).
    std::array<WGPUBindGroupLayoutEntry, 12> entries{};
    entries[0] = uniformEntry;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].texture.sampleType = WGPUTextureSampleType_Depth;
    entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Fragment;
    entries[2].sampler.type = WGPUSamplerBindingType_Comparison;
    entries[6].binding = 6;
    entries[6].visibility = WGPUShaderStage_Fragment;
    entries[6].texture.sampleType = WGPUTextureSampleType_Depth;
    entries[6].texture.viewDimension = WGPUTextureViewDimension_CubeArray;
    for (uint32_t binding = 3; binding <= 5; ++binding)
      entries[binding] =
          wgpu::storageEntry(binding,
                             WGPUShaderStage_Fragment,
                             WGPUBufferBindingType_ReadOnlyStorage);
    for (const uint32_t binding : {7u, 9u, 10u})
    {
      entries[binding].binding = binding;
      entries[binding].visibility = WGPUShaderStage_Fragment;
      entries[binding].texture.sampleType = WGPUTextureSampleType_Float;
      entries[binding].texture.viewDimension = WGPUTextureViewDimension_2D;
    }
    entries[8].binding = 8;
    entries[8].visibility = WGPUShaderStage_Fragment;
    entries[8].sampler.type = WGPUSamplerBindingType_Filtering;
    entries[11].binding = 11;
    entries[11].visibility = WGPUShaderStage_Fragment;
    entries[11].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    entries[11].texture.viewDimension = WGPUTextureViewDimension_2D;

    WGPUBindGroupLayoutDescriptor desc = {};
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    m_litLayout.reset(wgpuDeviceCreateBindGroupLayout(device, &desc));
  }

  // Bind groups. The shadow pass's is fixed for the renderer's lifetime (the
  // uniform buffer never changes); the lit pass's is rebuilt when the light
  // buffers grow.
  {
    std::array<WGPUBindGroupEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].buffer = m_frameUniforms.get();
    entries[0].size = sizeof(FrameUniforms);
    entries[1].binding = 1;
    entries[1].buffer = m_cascadeUniforms.get();
    entries[1].size = sizeof(glm::mat4);

    WGPUBindGroupDescriptor desc = {};
    desc.layout = m_shadowLayout.get();
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    m_shadowBindings.reset(wgpuDeviceCreateBindGroup(device, &desc));
  }
  // The point-shadow pass's group 0: the frame, and the face being drawn (at
  // a dynamic offset).
  {
    std::array<WGPUBindGroupLayoutEntry, 2> entries{};
    entries[0] = uniformEntry;
    entries[1] =
        wgpu::storageEntry(1,
                           WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
                           WGPUBufferBindingType_Uniform);
    entries[1].buffer.hasDynamicOffset = true;
    entries[1].buffer.minBindingSize = sizeof(FaceUniforms);
    WGPUBindGroupLayoutDescriptor desc = {};
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    m_pointShadowLayout.reset(wgpuDeviceCreateBindGroupLayout(device, &desc));
  }
  // Grass's group 2: a list of its grown blades, its settings, where it's
  // flattened, and its maps (data for its material, coverage for its
  // ground).
  {
    std::array<WGPUBindGroupLayoutEntry, 6> entries{};
    entries[0] = wgpu::storageEntry(
        0, WGPUShaderStage_Vertex, WGPUBufferBindingType_ReadOnlyStorage);
    const WGPUShaderStage both =
        WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[1] = wgpu::storageEntry(1, both, WGPUBufferBindingType_Uniform);
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Vertex;
    entries[2].texture.sampleType = WGPUTextureSampleType_Float;
    entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[3].binding = 3;
    entries[3].visibility = both;
    entries[3].sampler.type = WGPUSamplerBindingType_Filtering;
    entries[4].binding = 4;
    entries[4].visibility = both;
    entries[4].texture.sampleType = WGPUTextureSampleType_Float;
    entries[4].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[5].binding = 5;
    entries[5].visibility = WGPUShaderStage_Fragment;
    entries[5].texture.sampleType = WGPUTextureSampleType_Float;
    entries[5].texture.viewDimension = WGPUTextureViewDimension_2D;
    WGPUBindGroupLayoutDescriptor desc = {};
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    m_grassLayout.reset(wgpuDeviceCreateBindGroupLayout(device, &desc));
  }
  // Growing grass: its settings, ground and maps in, its blades and their
  // draws out.
  {
    std::array<WGPUBindGroupLayoutEntry, 6> entries{};
    entries[0] = wgpu::storageEntry(
        0, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform);
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Compute;
    entries[1].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Compute;
    entries[2].texture.sampleType = WGPUTextureSampleType_Float;
    entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[3].binding = 3;
    entries[3].visibility = WGPUShaderStage_Compute;
    entries[3].sampler.type = WGPUSamplerBindingType_Filtering;
    entries[4] = wgpu::storageEntry(
        4, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage);
    entries[5] = wgpu::storageEntry(
        5, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage);
    WGPUBindGroupLayoutDescriptor desc = {};
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    m_grassGrowLayout.reset(wgpuDeviceCreateBindGroupLayout(device, &desc));

    ValidationScope scope(device);
    const wgpu::ShaderModule module(wgpu::makeShader(
        device, std::string(shaders::meshGrassGrowWgsl).c_str()));
    const WGPUBindGroupLayout groups[] = {m_grassGrowLayout.get()};
    WGPUPipelineLayoutDescriptor layoutDesc = {};
    layoutDesc.bindGroupLayoutCount = 1;
    layoutDesc.bindGroupLayouts = groups;
    const wgpu::PipelineLayout layout(
        wgpuDeviceCreatePipelineLayout(device, &layoutDesc));
    WGPUComputePipelineDescriptor pipeline = {};
    pipeline.layout = layout.get();
    pipeline.compute.module = module.get();
    pipeline.compute.entryPoint = wgpu::sv("cs_main");
    m_grassGrowPipeline.reset(
        wgpuDeviceCreateComputePipeline(device, &pipeline));
    if (const std::optional<std::string> error = scope.end())
    {
      LOG_ERROR("Grass's grow pass doesn't build: " + *error);
      m_grassGrowPipeline.reset();
    }
  }
  // A physical sky's (failing, logged: its flat colour behind, its diffuse
  // light for reflections).
  if (!createSkyResources())
    LOG_ERROR("A physical sky's passes don't build: it draws without its "
              "reflections");
  ensurePointShadowMaps(1, 16); // until a light needs more
  ensureLightBuffers(1, 1, 1);
  if (!createPostPipelines() || !createUiPipeline())
    return false;

  // Group 1 of the standard surface's pipelines: the section's material.
  std::array<WGPUBindGroupLayoutEntry, 5> entries{};
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
  entries[4].binding = 4; // normal map
  entries[4].visibility = WGPUShaderStage_Fragment;
  entries[4].texture.sampleType = WGPUTextureSampleType_Float;
  entries[4].texture.viewDimension = WGPUTextureViewDimension_2D;
  WGPUBindGroupLayoutDescriptor layout = {};
  layout.entryCount = entries.size();
  layout.entries = entries.data();
  m_standard = buildSurface(
      std::string(shaders::meshStandardSurfaceWgsl),
      wgpu::BindGroupLayout(wgpuDeviceCreateBindGroupLayout(device, &layout)));

  return m_shadowLayout && m_litLayout && m_shadowBindings && m_litBindings &&
         m_standard;
}

std::shared_ptr<WebGpuRenderer::Surface>
WebGpuRenderer::buildSurface(const std::string& surfaceWgsl,
                             wgpu::BindGroupLayout materialLayout)
{
  const WGPUDevice device = m_context.device();
  if (!materialLayout)
    return nullptr;
  ValidationScope scope(device);

  const MeshVertexLayouts layouts;
  // Group 0: the frame (and, lit, the shadow map); group 1: the section's
  // material.
  const auto pipelineLayout = [&](WGPUBindGroupLayout frame)
  {
    const std::array<WGPUBindGroupLayout, 2> groups = {
        frame, materialLayout.get()};
    WGPUPipelineLayoutDescriptor desc = {};
    desc.bindGroupLayoutCount = groups.size();
    desc.bindGroupLayouts = groups.data();
    return wgpu::PipelineLayout(wgpuDeviceCreatePipelineLayout(device, &desc));
  };

  auto surface = std::make_shared<Surface>();
  surface->wgsl = surfaceWgsl;

  // Shadow pipeline: depth only, with a fragment stage that cuts out where a
  // masked or see-through material isn't solid. No culling, so open meshes
  // (planes, roof undersides) still cast; slope-scaled bias keeps sloped faces
  // clean.
  {
    const wgpu::ShaderModule module(wgpu::makeShader(
        device,
        shaders::meshPass(surfaceWgsl, shaders::meshShadowWgsl).c_str()));
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
    surface->shadow.reset(wgpuDeviceCreateRenderPipeline(device, &desc));
  }

  // Point-light shadow pipeline: as the sun's, but it writes each surface's
  // distance from the light as its depth (so no depth bias: the lit pass
  // offsets instead).
  {
    const wgpu::ShaderModule module(wgpu::makeShader(
        device,
        shaders::meshPass(surfaceWgsl, shaders::meshPointShadowWgsl).c_str()));
    const wgpu::PipelineLayout layout =
        pipelineLayout(m_pointShadowLayout.get());

    const WGPUDepthStencilState depth =
        depthState(true, WGPUCompareFunction_Less);

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
    surface->pointShadow.reset(wgpuDeviceCreateRenderPipeline(device, &desc));
  }

  // Lit pipelines: back faces culled. Opaque ones test and write depth;
  // see-through ones test it, blend over what's there and leave depth alone.
  {
    const wgpu::ShaderModule module(wgpu::makeShader(
        device,
        shaders::meshPass(
            surfaceWgsl, shaders::litPass(shaders::meshLitVertexWgsl))
            .c_str()));
    const wgpu::PipelineLayout layout = pipelineLayout(m_litLayout.get());

    // How a pipeline puts its colour down: over what's there, blended by
    // alpha, or added to it.
    enum class Blending
    {
      None,
      Alpha,
      Add,
    };
    const auto build = [&](Blending kind)
    {
      const bool blend = kind != Blending::None;
      const WGPUDepthStencilState depth =
          depthState(!blend, WGPUCompareFunction_Less);

      // Colour blends by alpha; the target's alpha stays 1 (the editor shows
      // the viewport as an image, which a lower alpha would let show through).
      WGPUBlendState blending = {};
      blending.color = {WGPUBlendOperation_Add,
                        WGPUBlendFactor_SrcAlpha,
                        kind == Blending::Add
                            ? WGPUBlendFactor_One
                            : WGPUBlendFactor_OneMinusSrcAlpha};
      blending.alpha = {
          WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One};

      // The scene, and the light that blooms (see ensureTargets()).
      std::array<WGPUColorTargetState, 2> colors{};
      for (WGPUColorTargetState& color : colors)
      {
        color.format = kSceneFormat;
        color.writeMask = WGPUColorWriteMask_All;
        color.blend = blend ? &blending : nullptr;
      }

      WGPUFragmentState fragment = {};
      fragment.module = module.get();
      fragment.entryPoint = wgpu::sv("fs_main");
      fragment.targetCount = colors.size();
      fragment.targets = colors.data();

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
      desc.multisample.count = m_sampleCount;
      desc.multisample.mask = 0xFFFFFFFFu;
      desc.fragment = &fragment;
      return wgpu::RenderPipeline(
          wgpuDeviceCreateRenderPipeline(device, &desc));
    };
    surface->lit = build(Blending::None);
    surface->blend = build(Blending::Alpha);
    surface->additive = build(Blending::Add);
  }

  if (const std::optional<std::string> error = scope.end())
  {
    LOG_ERROR("A material's shader doesn't build: " + *error);
    return nullptr;
  }
  if (!surface->shadow || !surface->lit || !surface->blend ||
      !surface->additive || !surface->pointShadow)
    return nullptr;
  surface->materialLayout = std::move(materialLayout);
  return surface;
}

std::shared_ptr<const WebGpuRenderer::Surface>
WebGpuRenderer::graphSurface(const materialGraph::MaterialShader& shader,
                             AlphaMode mode)
{
  const std::string wgsl = shaders::graphSurface(shader, mode);
  if (const auto it = m_surfaces.find(wgsl); it != m_surfaces.end())
    if (std::shared_ptr<const Surface> live = it->second.lock())
      return live;
  if (m_brokenSurfaces.contains(wgsl))
    return nullptr;

  std::shared_ptr<const Surface> surface = buildSurface(
      wgsl, graphMaterialLayout(m_context.device(), shader.textureCount()));
  if (!surface)
  {
    m_brokenSurfaces.insert(wgsl);
    return nullptr;
  }
  // Forget surfaces no material uses any more.
  std::erase_if(
      m_surfaces, [](const auto& entry) { return entry.second.expired(); });
  m_surfaces[wgsl] = surface;
  return surface;
}

void WebGpuRenderer::ensureTargets()
{
  if (m_depth && m_depthSize == m_viewport)
    return;
  const WGPUDevice device = m_context.device();

  // Multisampled, as the lit pass draws; depth of field reads it after.
  m_depthView = makeDepthTexture(
      device,
      m_viewport,
      WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
      m_depth,
      m_sampleCount);
  m_depthSize = m_viewport;

  const auto colorTexture = [&](glm::ivec2 size, wgpu::Texture& texture)
  {
    WGPUTextureDescriptor desc = {};
    desc.usage =
        WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = {
        static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), 1};
    desc.format = kSceneFormat;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    texture.reset(wgpuDeviceCreateTexture(device, &desc));
    return wgpu::TextureView(wgpuTextureCreateView(texture.get(), nullptr));
  };
  m_sceneView = colorTexture(m_viewport, m_scene);
  m_glowSourceView = colorTexture(m_viewport, m_glowSource);
  // What the lit pass draws into, a few samples a pixel, resolved into those.
  const auto multisampled = [&](wgpu::Texture& texture)
  {
    WGPUTextureDescriptor desc = {};
    desc.usage = WGPUTextureUsage_RenderAttachment;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = {static_cast<uint32_t>(m_viewport.x),
                 static_cast<uint32_t>(m_viewport.y),
                 1};
    desc.format = kSceneFormat;
    desc.mipLevelCount = 1;
    desc.sampleCount = m_sampleCount;
    texture.reset(wgpuDeviceCreateTexture(device, &desc));
    return wgpu::TextureView(wgpuTextureCreateView(texture.get(), nullptr));
  };
  m_sceneSamplesView = multisampled(m_sceneSamples);
  m_glowSamplesView = multisampled(m_glowSamples);

  // Bloom's levels, each half the last.
  m_bloom.clear();
  glm::ivec2 size = m_viewport;
  while (static_cast<int>(m_bloom.size()) < kBloomLevels)
  {
    size = glm::max(size / 2, glm::ivec2(1));
    if (!m_bloom.empty() && std::min(size.x, size.y) < kBloomSmallest)
      break;
    BloomLevel& level = m_bloom.emplace_back();
    level.size = size;
    level.view = colorTexture(size, level.texture);
  }

  // Each pass's parameters and bindings: a source, the sampler, the
  // parameters and (only the composite reads it) the glow.
  const std::shared_ptr<GpuTexture> black =
      solidTexture(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
  const auto bindings =
      [&](WGPUTextureView source, wgpu::Buffer& params, WGPUTextureView glow)
  {
    params = makeBuffer(device,
                        sizeof(PostUniforms),
                        WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    std::array<WGPUBindGroupEntry, 4> entries{};
    entries[0].binding = 0;
    entries[0].textureView = source;
    entries[1].binding = 1;
    entries[1].sampler = m_clampSampler.get();
    entries[2].binding = 2;
    entries[2].buffer = params.get();
    entries[2].size = sizeof(PostUniforms);
    entries[3].binding = 3;
    entries[3].textureView = glow;
    WGPUBindGroupDescriptor desc = {};
    desc.layout = m_postLayout.get();
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    return wgpu::BindGroup(wgpuDeviceCreateBindGroup(device, &desc));
  };
  for (std::size_t i = 0; i < m_bloom.size(); ++i)
  {
    BloomLevel& level = m_bloom[i];
    level.down =
        bindings(i == 0 ? m_glowSourceView.get() : m_bloom[i - 1].view.get(),
                 level.downParams,
                 black->view.get());
    if (i + 1 < m_bloom.size())
      level.up = bindings(
          m_bloom[i + 1].view.get(), level.upParams, black->view.get());
  }
  m_compositeBindings =
      bindings(m_sceneView.get(),
               m_compositeParams,
               m_bloom.empty() ? black->view.get() : m_bloom[0].view.get());

  // Depth of field: half-size targets, and the passes' bindings (a source,
  // the sampler, the parameters, the depth, and what fs_combine mixes in).
  const glm::ivec2 half = glm::max(m_viewport / 2, glm::ivec2(1));
  m_focusHalf.view = colorTexture(half, m_focusHalf.texture);
  m_focusBlur.view = colorTexture(half, m_focusBlur.texture);
  m_focusOut.view = colorTexture(m_viewport, m_focusOut.texture);
  m_focusParams = makeBuffer(device,
                             sizeof(FocusUniforms),
                             WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  const auto focusBindings = [&](WGPUTextureView source, WGPUTextureView mixIn)
  {
    std::array<WGPUBindGroupEntry, 5> entries{};
    entries[0].binding = 0;
    entries[0].textureView = source;
    entries[1].binding = 1;
    entries[1].sampler = m_clampSampler.get();
    entries[2].binding = 2;
    entries[2].buffer = m_focusParams.get();
    entries[2].size = sizeof(FocusUniforms);
    entries[3].binding = 3;
    entries[3].textureView = m_depthView.get();
    entries[4].binding = 4;
    entries[4].textureView = mixIn;
    WGPUBindGroupDescriptor desc = {};
    desc.layout = m_focusLayout.get();
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    return wgpu::BindGroup(wgpuDeviceCreateBindGroup(device, &desc));
  };
  if (m_capabilities.depthOfField)
  {
    m_focusPrepareBindings =
        focusBindings(m_sceneView.get(), black->view.get());
    m_focusBlurBindings =
        focusBindings(m_focusHalf.view.get(), black->view.get());
    m_focusCombineBindings =
        focusBindings(m_sceneView.get(), m_focusBlur.view.get());
  }
  // The composite of the scene through it: as the plain one, from its output.
  {
    std::array<WGPUBindGroupEntry, 4> entries{};
    entries[0].binding = 0;
    entries[0].textureView = m_focusOut.view.get();
    entries[1].binding = 1;
    entries[1].sampler = m_clampSampler.get();
    entries[2].binding = 2;
    entries[2].buffer = m_compositeParams.get();
    entries[2].size = sizeof(PostUniforms);
    entries[3].binding = 3;
    entries[3].textureView =
        m_bloom.empty() ? black->view.get() : m_bloom[0].view.get();
    WGPUBindGroupDescriptor desc = {};
    desc.layout = m_postLayout.get();
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    m_compositeFocusBindings.reset(wgpuDeviceCreateBindGroup(device, &desc));
  }
}

void WebGpuRenderer::ensureLightBuffers(std::size_t lights,
                                        std::size_t clusters,
                                        std::size_t indices)
{
  // Grown geometrically (never empty: storage bindings need some size).
  const auto grow = [&](wgpu::Buffer& buffer,
                        std::size_t& capacity,
                        std::size_t wanted,
                        std::size_t elementSize)
  {
    if (buffer && wanted <= capacity)
      return false;
    capacity = std::max({wanted, capacity * 2, std::size_t{64}});
    buffer = makeBuffer(m_context.device(),
                        capacity * elementSize,
                        WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
    return true;
  };
  bool grew =
      grow(m_lightBuffer, m_lightCapacity, lights, sizeof(GpuPointLight));
  grew |= grow(m_clusterRangeBuffer,
               m_clusterRangeCapacity,
               clusters,
               sizeof(glm::uvec2));
  grew |= grow(
      m_clusterIndexBuffer, m_clusterIndexCapacity, indices, sizeof(uint32_t));
  if (grew)
    rebuildLitBindings();
}

void WebGpuRenderer::rebuildLitBindings()
{
  // Not until the light buffers and shadow maps are all there.
  if (!m_lightBuffer || !m_clusterRangeBuffer || !m_clusterIndexBuffer ||
      !m_pointShadowCubes || !m_skyReflections.view || !m_brdfTableView ||
      !m_noOcclusionView)
    return;
  std::array<WGPUBindGroupEntry, 12> entries{};
  entries[0].binding = 0;
  entries[0].buffer = m_frameUniforms.get();
  entries[0].size = sizeof(FrameUniforms);
  entries[1].binding = 1;
  entries[1].textureView = m_shadowView.get();
  entries[2].binding = 2;
  entries[2].sampler = m_shadowSampler.get();
  entries[3].binding = 3;
  entries[3].buffer = m_lightBuffer.get();
  entries[3].size = m_lightCapacity * sizeof(GpuPointLight);
  entries[4].binding = 4;
  entries[4].buffer = m_clusterRangeBuffer.get();
  entries[4].size = m_clusterRangeCapacity * sizeof(glm::uvec2);
  entries[5].binding = 5;
  entries[5].buffer = m_clusterIndexBuffer.get();
  entries[5].size = m_clusterIndexCapacity * sizeof(uint32_t);
  entries[6].binding = 6;
  entries[6].textureView = m_pointShadowCubes.get();
  entries[7].binding = 7;
  entries[7].textureView = m_skyReflections.view.get();
  entries[8].binding = 8;
  entries[8].sampler = m_skySampler.get();
  entries[9].binding = 9;
  entries[9].textureView = m_skyRadiance.view.get();
  entries[10].binding = 10;
  entries[10].textureView = m_brdfTableView.get();
  entries[11].binding = 11;
  entries[11].textureView = m_occlusion.occlusionView
                                ? m_occlusion.occlusionView.get()
                                : m_noOcclusionView.get();

  WGPUBindGroupDescriptor desc = {};
  desc.layout = m_litLayout.get();
  desc.entryCount = entries.size();
  desc.entries = entries.data();
  m_litBindings.reset(wgpuDeviceCreateBindGroup(m_context.device(), &desc));
}

void WebGpuRenderer::ensurePointShadowMaps(uint32_t lights, uint32_t resolution)
{
  // Room for a power of two of lights, so it's remade only now and then.
  uint32_t capacity = 1;
  while (capacity < lights && capacity < kMaxShadowedPointLights)
    capacity *= 2;
  resolution = std::clamp(resolution, 16u, 2048u);
  if (m_pointShadowMap && capacity <= m_pointShadowCapacity &&
      resolution == m_pointShadowResolution)
    return;
  capacity = std::max(capacity, m_pointShadowCapacity);
  const WGPUDevice device = m_context.device();

  WGPUTextureDescriptor texture = {};
  texture.usage =
      WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
  texture.dimension = WGPUTextureDimension_2D;
  texture.size = {resolution, resolution, capacity * 6};
  texture.format = kDepthFormat;
  texture.mipLevelCount = 1;
  texture.sampleCount = 1;
  m_pointShadowMap.reset(wgpuDeviceCreateTexture(device, &texture));

  WGPUTextureViewDescriptor cubes = {};
  cubes.format = kDepthFormat;
  cubes.dimension = WGPUTextureViewDimension_CubeArray;
  cubes.mipLevelCount = 1;
  cubes.arrayLayerCount = capacity * 6;
  cubes.aspect = WGPUTextureAspect_DepthOnly;
  m_pointShadowCubes.reset(
      wgpuTextureCreateView(m_pointShadowMap.get(), &cubes));
  m_pointShadowFaces.clear();
  for (uint32_t layer = 0; layer < capacity * 6; ++layer)
  {
    WGPUTextureViewDescriptor face = {};
    face.format = kDepthFormat;
    face.dimension = WGPUTextureViewDimension_2D;
    face.baseArrayLayer = layer;
    face.arrayLayerCount = 1;
    face.mipLevelCount = 1;
    face.aspect = WGPUTextureAspect_DepthOnly;
    m_pointShadowFaces.emplace_back(
        wgpuTextureCreateView(m_pointShadowMap.get(), &face));
  }
  m_pointShadowCapacity = capacity;
  m_pointShadowResolution = resolution;

  m_faceUniforms =
      makeBuffer(device,
                 kFaceStride * capacity * 6,
                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  std::array<WGPUBindGroupEntry, 2> entries{};
  entries[0].binding = 0;
  entries[0].buffer = m_frameUniforms.get();
  entries[0].size = sizeof(FrameUniforms);
  entries[1].binding = 1;
  entries[1].buffer = m_faceUniforms.get();
  entries[1].size = sizeof(FaceUniforms);
  WGPUBindGroupDescriptor desc = {};
  desc.layout = m_pointShadowLayout.get();
  desc.entryCount = entries.size();
  desc.entries = entries.data();
  m_pointShadowBindings.reset(wgpuDeviceCreateBindGroup(device, &desc));

  rebuildLitBindings();
}

void WebGpuRenderer::encodePointShadows()
{
  const auto& shadowed = m_plan.shadowedLights();
  if (shadowed.empty())
    return;

  // Every face's view, in one write.
  const std::vector<FaceUniforms>& views = m_plan.faces();
  std::vector<uint8_t> faces(kFaceStride * views.size());
  for (std::size_t i = 0; i < views.size(); ++i)
    std::memcpy(faces.data() + kFaceStride * i, &views[i], sizeof(views[i]));
  wgpuQueueWriteBuffer(
      m_context.queue(), m_faceUniforms.get(), 0, faces.data(), faces.size());

  for (std::size_t i = 0; i < shadowed.size(); ++i)
    for (std::size_t face = 0; face < 6; ++face)
    {
      WGPURenderPassDepthStencilAttachment depth = {};
      depth.view = m_pointShadowFaces[i * 6 + face].get();
      depth.depthLoadOp = WGPULoadOp_Clear;
      depth.depthStoreOp = WGPUStoreOp_Store;
      depth.depthClearValue = 1.0f; // nothing in the way: all its reach
      WGPURenderPassDescriptor desc = {};
      desc.depthStencilAttachment = &depth;

      WGPURenderPassEncoder pass =
          wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
      m_boundPipeline = nullptr;
      const auto offset = static_cast<uint32_t>(kFaceStride * (i * 6 + face));
      wgpuRenderPassEncoderSetBindGroup(
          pass, 0, m_pointShadowBindings.get(), 1, &offset);
      if (!shadowed[i].draws.empty())
        wgpuRenderPassEncoderSetVertexBuffer(
            pass, 1, m_instanceBuffer.get(), 0, WGPU_WHOLE_SIZE);
      for (const Draw& draw : shadowed[i].draws)
        encodeDraw(pass, draw, Sections::LightShadow);
      endPass(pass);
    }
}

void WebGpuRenderer::uploadLights()
{
  // The lights, and each cluster's list of the ones that reach it.
  const std::vector<GpuPointLight>& lights = m_plan.pointLights();
  const ClusterLights& clusters = m_plan.clusters();
  ensureLightBuffers(
      lights.size(), clusters.ranges.size(), clusters.indices.size());
  const WGPUQueue queue = m_context.queue();
  if (!lights.empty())
    wgpuQueueWriteBuffer(queue,
                         m_lightBuffer.get(),
                         0,
                         lights.data(),
                         lights.size() * sizeof(GpuPointLight));
  wgpuQueueWriteBuffer(queue,
                       m_clusterRangeBuffer.get(),
                       0,
                       clusters.ranges.data(),
                       clusters.ranges.size() * sizeof(glm::uvec2));
  if (!clusters.indices.empty())
    wgpuQueueWriteBuffer(queue,
                         m_clusterIndexBuffer.get(),
                         0,
                         clusters.indices.data(),
                         clusters.indices.size() * sizeof(uint32_t));
}

bool WebGpuRenderer::createPostPipelines()
{
  const WGPUDevice device = m_context.device();

  // Bloom samples between texels, clamped at the edges.
  WGPUSamplerDescriptor sampler = {};
  sampler.addressModeU = WGPUAddressMode_ClampToEdge;
  sampler.addressModeV = WGPUAddressMode_ClampToEdge;
  sampler.addressModeW = WGPUAddressMode_ClampToEdge;
  sampler.magFilter = WGPUFilterMode_Linear;
  sampler.minFilter = WGPUFilterMode_Linear;
  sampler.mipmapFilter = WGPUMipmapFilterMode_Nearest;
  sampler.maxAnisotropy = 1;
  m_clampSampler.reset(wgpuDeviceCreateSampler(device, &sampler));

  std::array<WGPUBindGroupLayoutEntry, 4> entries{};
  entries[0].binding = 0;
  entries[0].visibility = WGPUShaderStage_Fragment;
  entries[0].texture.sampleType = WGPUTextureSampleType_Float;
  entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
  entries[1].binding = 1;
  entries[1].visibility = WGPUShaderStage_Fragment;
  entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
  entries[2] = wgpu::storageEntry(
      2, WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform);
  entries[3].binding = 3;
  entries[3].visibility = WGPUShaderStage_Fragment;
  entries[3].texture.sampleType = WGPUTextureSampleType_Float;
  entries[3].texture.viewDimension = WGPUTextureViewDimension_2D;
  WGPUBindGroupLayoutDescriptor layoutDesc = {};
  layoutDesc.entryCount = entries.size();
  layoutDesc.entries = entries.data();
  m_postLayout.reset(wgpuDeviceCreateBindGroupLayout(device, &layoutDesc));

  const WGPUBindGroupLayout groups[] = {m_postLayout.get()};
  WGPUPipelineLayoutDescriptor pipelineLayoutDesc = {};
  pipelineLayoutDesc.bindGroupLayoutCount = 1;
  pipelineLayoutDesc.bindGroupLayouts = groups;
  const wgpu::PipelineLayout layout(
      wgpuDeviceCreatePipelineLayout(device, &pipelineLayoutDesc));
  const wgpu::ShaderModule module(
      wgpu::makeShader(device, std::string(shaders::postWgsl).c_str()));

  const auto build = [&](const char* entry, WGPUTextureFormat format, bool add)
  {
    // Upsampling adds onto the level it draws into.
    WGPUBlendState adding = {};
    adding.color = {
        WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One};
    adding.alpha = {
        WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One};
    WGPUColorTargetState color = {};
    color.format = format;
    color.writeMask = WGPUColorWriteMask_All;
    color.blend = add ? &adding : nullptr;

    WGPUFragmentState fragment = {};
    fragment.module = module.get();
    fragment.entryPoint = wgpu::sv(entry);
    fragment.targetCount = 1;
    fragment.targets = &color;

    WGPURenderPipelineDescriptor desc = {};
    desc.layout = layout.get();
    desc.vertex.module = module.get();
    desc.vertex.entryPoint = wgpu::sv("vs_main");
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.primitive.cullMode = WGPUCullMode_None;
    desc.multisample.count = 1;
    desc.multisample.mask = 0xFFFFFFFFu;
    desc.fragment = &fragment;
    return wgpu::RenderPipeline(wgpuDeviceCreateRenderPipeline(device, &desc));
  };
  m_prefilterPipeline = build("fs_prefilter", kSceneFormat, false);
  m_downsamplePipeline = build("fs_downsample", kSceneFormat, false);
  m_upsamplePipeline = build("fs_upsample", kSceneFormat, true);
  m_compositePipeline = build("fs_composite", m_context.format(), false);

  // Depth of field's passes: as bloom's, with the depth buffer and the blur
  // to mix in. Not on OpenGL, whose shaders can't read a depth buffer's
  // samples.
  m_capabilities.depthOfField = !m_context.onOpenGL();
  if (m_capabilities.depthOfField)
  {
    std::array<WGPUBindGroupLayoutEntry, 5> focus{};
    focus[0] = entries[0];
    focus[1] = entries[1];
    focus[2] = entries[2];
    focus[3].binding = 3;
    focus[3].visibility = WGPUShaderStage_Fragment;
    focus[3].texture.sampleType = WGPUTextureSampleType_Depth;
    focus[3].texture.viewDimension = WGPUTextureViewDimension_2D;
    focus[3].texture.multisampled = m_sampleCount > 1;
    focus[4] = entries[3];
    focus[4].binding = 4;
    WGPUBindGroupLayoutDescriptor focusDesc = {};
    focusDesc.entryCount = focus.size();
    focusDesc.entries = focus.data();
    m_focusLayout.reset(wgpuDeviceCreateBindGroupLayout(device, &focusDesc));

    const WGPUBindGroupLayout focusGroups[] = {m_focusLayout.get()};
    WGPUPipelineLayoutDescriptor focusLayoutDesc = {};
    focusLayoutDesc.bindGroupLayoutCount = 1;
    focusLayoutDesc.bindGroupLayouts = focusGroups;
    const wgpu::PipelineLayout focusLayout(
        wgpuDeviceCreatePipelineLayout(device, &focusLayoutDesc));
    const wgpu::ShaderModule focusModule(
        wgpu::makeShader(device, std::string(shaders::postFocusWgsl).c_str()));
    const auto focusPipeline = [&](const char* entry)
    {
      WGPUColorTargetState color = {};
      color.format = kSceneFormat;
      color.writeMask = WGPUColorWriteMask_All;
      WGPUFragmentState fragment = {};
      fragment.module = focusModule.get();
      fragment.entryPoint = wgpu::sv(entry);
      fragment.targetCount = 1;
      fragment.targets = &color;
      WGPURenderPipelineDescriptor desc = {};
      desc.layout = focusLayout.get();
      desc.vertex.module = focusModule.get();
      desc.vertex.entryPoint = wgpu::sv("vs_main");
      desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
      desc.primitive.cullMode = WGPUCullMode_None;
      desc.multisample.count = 1;
      desc.multisample.mask = 0xFFFFFFFFu;
      desc.fragment = &fragment;
      return wgpu::RenderPipeline(
          wgpuDeviceCreateRenderPipeline(device, &desc));
    };
    m_focusPreparePipeline = focusPipeline("fs_prepare");
    m_focusBlurPipeline = focusPipeline("fs_blur");
    m_focusCombinePipeline = focusPipeline("fs_combine");
  }
  return m_clampSampler && m_postLayout && m_prefilterPipeline &&
         m_downsamplePipeline && m_upsamplePipeline && m_compositePipeline &&
         (!m_capabilities.depthOfField ||
          (m_focusLayout && m_focusPreparePipeline && m_focusBlurPipeline &&
           m_focusCombinePipeline));
}

void WebGpuRenderer::drawFullscreen(WGPUTextureView target,
                                    WGPURenderPipeline pipeline,
                                    WGPUBindGroup bindings,
                                    bool blend)
{
  WGPURenderPassColorAttachment color = {};
  color.view = target;
  color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  color.loadOp = blend ? WGPULoadOp_Load : WGPULoadOp_Clear;
  color.storeOp = WGPUStoreOp_Store;
  color.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
  WGPURenderPassDescriptor desc = {};
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color;
  WGPURenderPassEncoder pass =
      wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
  wgpuRenderPassEncoderSetPipeline(pass, pipeline);
  wgpuRenderPassEncoderSetBindGroup(pass, 0, bindings, 0, nullptr);
  wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
  endPass(pass);
}

void WebGpuRenderer::encodeBloom()
{
  const auto write = [&](const wgpu::Buffer& buffer, glm::ivec2 sourceSize)
  {
    const PostUniforms uniforms = m_plan.bloomUniforms(sourceSize);
    wgpuQueueWriteBuffer(
        m_context.queue(), buffer.get(), 0, &uniforms, sizeof(uniforms));
  };

  // Down: the blooming light above the threshold into level 0, then each
  // level halved into the next.
  for (std::size_t i = 0; i < m_bloom.size(); ++i)
  {
    write(m_bloom[i].downParams, i == 0 ? m_viewport : m_bloom[i - 1].size);
    drawFullscreen(
        m_bloom[i].view.get(),
        i == 0 ? m_prefilterPipeline.get() : m_downsamplePipeline.get(),
        m_bloom[i].down.get(),
        false);
  }
  // Up: each level spread and added onto the one above, back to level 0.
  for (std::size_t i = m_bloom.size() - 1; i-- > 0;)
  {
    write(m_bloom[i].upParams, m_bloom[i + 1].size);
    drawFullscreen(m_bloom[i].view.get(),
                   m_upsamplePipeline.get(),
                   m_bloom[i].up.get(),
                   true);
  }
}

void WebGpuRenderer::encodeComposite()
{
  // The scene plus the glow (none when bloom is off), into the target.
  const PostUniforms uniforms = m_plan.compositeUniforms(m_viewport);
  wgpuQueueWriteBuffer(m_context.queue(),
                       m_compositeParams.get(),
                       0,
                       &uniforms,
                       sizeof(uniforms));
  drawFullscreen(
      m_target,
      m_compositePipeline.get(),
      m_focused ? m_compositeFocusBindings.get() : m_compositeBindings.get(),
      false);
}

bool WebGpuRenderer::createSkyResources()
{
  const WGPUDevice device = m_context.device();
  const WGPUQueue queue = m_context.queue();

  // The textures first: the lit pass binds them, physical sky or not.
  const auto target = [&](uint32_t size, uint32_t mips, SkyTarget& out)
  {
    WGPUTextureDescriptor desc = {};
    desc.usage =
        WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = {size, size, 1};
    desc.format = kSkyFormat;
    desc.mipLevelCount = mips;
    desc.sampleCount = 1;
    out.texture.reset(wgpuDeviceCreateTexture(device, &desc));
    out.view.reset(wgpuTextureCreateView(out.texture.get(), nullptr));
    out.mips.clear();
    for (uint32_t mip = 0; mip < mips; ++mip)
    {
      WGPUTextureViewDescriptor view = {};
      view.format = kSkyFormat;
      view.dimension = WGPUTextureViewDimension_2D;
      view.baseMipLevel = mip;
      view.mipLevelCount = 1;
      view.arrayLayerCount = 1;
      view.aspect = WGPUTextureAspect_All;
      out.mips.emplace_back(wgpuTextureCreateView(out.texture.get(), &view));
    }
  };
  target(kSkySize, 1, m_skyRadiance);
  target(kSkyReflectionSize, kSkyReflectionMips, m_skyReflections);

  const auto halfTexture =
      [&](glm::ivec2 size, wgpu::Texture& texture, wgpu::TextureView& view)
  {
    WGPUTextureDescriptor desc = {};
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = {
        static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), 1};
    desc.format = kSkyFormat;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    texture.reset(wgpuDeviceCreateTexture(device, &desc));
    view.reset(wgpuTextureCreateView(texture.get(), nullptr));
  };
  halfTexture(
      {PhysicalSky::kTransmittanceAngles, PhysicalSky::kTransmittanceHeights},
      m_skyTable,
      m_skyTableView);
  halfTexture({kBrdfTableSize, kBrdfTableSize}, m_brdfTable, m_brdfTableView);
  {
    const std::vector<glm::vec2> brdf = brdfTable(kBrdfTableSize);
    std::vector<uint64_t> texels(brdf.size());
    for (std::size_t i = 0; i < brdf.size(); ++i)
      texels[i] = glm::packHalf4x16(glm::vec4(brdf[i], 0.0f, 1.0f));
    writeTexture(queue,
                 m_brdfTable.get(),
                 {0, 0},
                 {kBrdfTableSize, kBrdfTableSize},
                 texels.data(),
                 sizeof(uint64_t));
  }

  WGPUSamplerDescriptor sampler = {};
  sampler.addressModeU = WGPUAddressMode_ClampToEdge;
  sampler.addressModeV = WGPUAddressMode_ClampToEdge;
  sampler.addressModeW = WGPUAddressMode_ClampToEdge;
  sampler.magFilter = WGPUFilterMode_Linear;
  sampler.minFilter = WGPUFilterMode_Linear;
  sampler.mipmapFilter = WGPUMipmapFilterMode_Linear;
  sampler.lodMinClamp = 0.0f;
  sampler.lodMaxClamp = 32.0f;
  sampler.maxAnisotropy = 1;
  m_skySampler.reset(wgpuDeviceCreateSampler(device, &sampler));
  if (!m_skyRadiance.view || !m_skyReflections.view || !m_skyTableView ||
      !m_brdfTableView || !m_skySampler)
    return false;

  // The passes' group 0: the atmosphere, a source (the transmittance table,
  // or the sky), its sampler, and the pass's own parameters.
  std::array<WGPUBindGroupLayoutEntry, 4> entries{};
  entries[0] = wgpu::storageEntry(
      0, WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform);
  entries[1].binding = 1;
  entries[1].visibility = WGPUShaderStage_Fragment;
  entries[1].texture.sampleType = WGPUTextureSampleType_Float;
  entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
  entries[2].binding = 2;
  entries[2].visibility = WGPUShaderStage_Fragment;
  entries[2].sampler.type = WGPUSamplerBindingType_Filtering;
  entries[3] = wgpu::storageEntry(
      3, WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform);
  WGPUBindGroupLayoutDescriptor layoutDesc = {};
  layoutDesc.entryCount = entries.size();
  layoutDesc.entries = entries.data();
  m_skyLayout.reset(wgpuDeviceCreateBindGroupLayout(device, &layoutDesc));

  m_skyUniforms = makeBuffer(device,
                             sizeof(SkyUniforms),
                             WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  m_skyPassUniforms.clear();
  m_skyPassBindings.clear();
  for (uint32_t pass = 0; pass <= kSkyReflectionMips; ++pass)
  {
    // Pass 0 renders the sky; pass 1 + k, reflection mip k.
    SkyPassUniforms params{};
    if (pass > 0)
      params.params =
          glm::vec4(static_cast<float>(pass - 1) / (kSkyReflectionMips - 1),
                    static_cast<float>(kSkySize),
                    kSkyReflectionSamples[pass - 1],
                    0.0f);
    wgpu::Buffer& buffer = m_skyPassUniforms.emplace_back(
        makeBuffer(device,
                   sizeof(SkyPassUniforms),
                   WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst));
    wgpuQueueWriteBuffer(queue, buffer.get(), 0, &params, sizeof(params));

    std::array<WGPUBindGroupEntry, 4> bindings{};
    bindings[0].binding = 0;
    bindings[0].buffer = m_skyUniforms.get();
    bindings[0].size = sizeof(SkyUniforms);
    bindings[1].binding = 1;
    bindings[1].textureView =
        pass == 0 ? m_skyTableView.get() : m_skyRadiance.view.get();
    bindings[2].binding = 2;
    bindings[2].sampler = m_skySampler.get();
    bindings[3].binding = 3;
    bindings[3].buffer = buffer.get();
    bindings[3].size = sizeof(SkyPassUniforms);
    WGPUBindGroupDescriptor desc = {};
    desc.layout = m_skyLayout.get();
    desc.entryCount = bindings.size();
    desc.entries = bindings.data();
    m_skyPassBindings.emplace_back(wgpuDeviceCreateBindGroup(device, &desc));
  }

  ValidationScope scope(device);
  {
    const WGPUBindGroupLayout groups[] = {m_skyLayout.get()};
    WGPUPipelineLayoutDescriptor pipelineLayout = {};
    pipelineLayout.bindGroupLayoutCount = 1;
    pipelineLayout.bindGroupLayouts = groups;
    const wgpu::PipelineLayout layout(
        wgpuDeviceCreatePipelineLayout(device, &pipelineLayout));
    const wgpu::ShaderModule module(
        wgpu::makeShader(device, shaders::skyPasses().c_str()));
    const auto build = [&](const char* entry)
    {
      WGPUColorTargetState color = {};
      color.format = kSkyFormat;
      color.writeMask = WGPUColorWriteMask_All;
      WGPUFragmentState fragment = {};
      fragment.module = module.get();
      fragment.entryPoint = wgpu::sv(entry);
      fragment.targetCount = 1;
      fragment.targets = &color;
      WGPURenderPipelineDescriptor desc = {};
      desc.layout = layout.get();
      desc.vertex.module = module.get();
      desc.vertex.entryPoint = wgpu::sv("vs_main");
      desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
      desc.primitive.cullMode = WGPUCullMode_None;
      desc.multisample.count = 1;
      desc.multisample.mask = 0xFFFFFFFFu;
      desc.fragment = &fragment;
      return wgpu::RenderPipeline(
          wgpuDeviceCreateRenderPipeline(device, &desc));
    };
    m_skyRadiancePipeline = build("fs_radiance");
    m_skyPrefilterPipeline = build("fs_prefilter");
  }
  // The background: in the lit pass, at the far plane, behind everything.
  {
    const WGPUBindGroupLayout groups[] = {m_litLayout.get()};
    WGPUPipelineLayoutDescriptor pipelineLayout = {};
    pipelineLayout.bindGroupLayoutCount = 1;
    pipelineLayout.bindGroupLayouts = groups;
    const wgpu::PipelineLayout layout(
        wgpuDeviceCreatePipelineLayout(device, &pipelineLayout));
    const wgpu::ShaderModule module(
        wgpu::makeShader(device, shaders::skyBackground().c_str()));
    std::array<WGPUColorTargetState, 2> colors{};
    for (WGPUColorTargetState& color : colors)
    {
      color.format = kSceneFormat;
      color.writeMask = WGPUColorWriteMask_All;
    }
    WGPUFragmentState fragment = {};
    fragment.module = module.get();
    fragment.entryPoint = wgpu::sv("fs_main");
    fragment.targetCount = colors.size();
    fragment.targets = colors.data();
    const WGPUDepthStencilState depth =
        depthState(false, WGPUCompareFunction_LessEqual);
    WGPURenderPipelineDescriptor desc = {};
    desc.layout = layout.get();
    desc.vertex.module = module.get();
    desc.vertex.entryPoint = wgpu::sv("vs_main");
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.primitive.cullMode = WGPUCullMode_None;
    desc.depthStencil = &depth;
    desc.multisample.count = m_sampleCount;
    desc.multisample.mask = 0xFFFFFFFFu;
    desc.fragment = &fragment;
    m_skyBackgroundPipeline.reset(
        wgpuDeviceCreateRenderPipeline(device, &desc));
  }
  if (const std::optional<std::string> error = scope.end())
  {
    LOG_ERROR("A physical sky's shaders don't build: " + *error);
    m_skyRadiancePipeline.reset();
    m_skyPrefilterPipeline.reset();
    m_skyBackgroundPipeline.reset();
    return false;
  }
  return m_skyLayout && m_skyRadiancePipeline && m_skyPrefilterPipeline &&
         m_skyBackgroundPipeline;
}

void WebGpuRenderer::encodeSky()
{
  const FrameSky& sky = m_plan.frame().sky;
  if (!sky.physical || !m_skyRadiancePipeline || !m_skyPrefilterPipeline)
    return;

  // A new atmosphere (its settings changed): its transmittance table.
  if (sky.physical != m_skyModel)
  {
    const std::vector<glm::vec4>& table = sky.physical->transmittanceTable();
    std::vector<uint64_t> texels(table.size());
    for (std::size_t i = 0; i < table.size(); ++i)
      texels[i] = glm::packHalf4x16(table[i]);
    writeTexture(
        m_context.queue(),
        m_skyTable.get(),
        {0, 0},
        {PhysicalSky::kTransmittanceAngles, PhysicalSky::kTransmittanceHeights},
        texels.data(),
        sizeof(uint64_t));
    m_skyModel = sky.physical;
    m_skyReady = false;
  }
  if (m_skyReady && sky.revision == m_skyRevision)
    return;

  // The sky, then each mip of its reflections from it.
  const SkyUniforms uniforms = m_plan.skyUniforms();
  wgpuQueueWriteBuffer(
      m_context.queue(), m_skyUniforms.get(), 0, &uniforms, sizeof(uniforms));
  drawFullscreen(m_skyRadiance.mips[0].get(),
                 m_skyRadiancePipeline.get(),
                 m_skyPassBindings[0].get(),
                 false);
  for (uint32_t mip = 0; mip < kSkyReflectionMips; ++mip)
    drawFullscreen(m_skyReflections.mips[mip].get(),
                   m_skyPrefilterPipeline.get(),
                   m_skyPassBindings[mip + 1].get(),
                   false);
  m_skyRevision = sky.revision;
  m_skyReady = true;
}

void WebGpuRenderer::encodeSkyBackground(WGPURenderPassEncoder pass)
{
  if (!m_plan.frame().sky.physical || !m_skyReady || !m_skyBackgroundPipeline)
    return;
  wgpuRenderPassEncoderSetPipeline(pass, m_skyBackgroundPipeline.get());
  m_boundPipeline = m_skyBackgroundPipeline.get();
  wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
}

void WebGpuRenderer::drawUi(const UiDrawList& list) { m_plan.addUi(list); }

bool WebGpuRenderer::createUiPipeline()
{
  const WGPUDevice device = m_context.device();
  {
    std::array<WGPUBindGroupLayoutEntry, 2> entries{};
    entries[0] =
        wgpu::storageEntry(0,
                           WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
                           WGPUBufferBindingType_Uniform);
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
    WGPUBindGroupLayoutDescriptor desc = {};
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    m_uiFrameLayout.reset(wgpuDeviceCreateBindGroupLayout(device, &desc));
  }
  {
    WGPUBindGroupLayoutEntry entry = {};
    entry.binding = 0;
    entry.visibility = WGPUShaderStage_Fragment;
    entry.texture.sampleType = WGPUTextureSampleType_Float;
    entry.texture.viewDimension = WGPUTextureViewDimension_2D;
    WGPUBindGroupLayoutDescriptor desc = {};
    desc.entryCount = 1;
    desc.entries = &entry;
    m_uiTextureLayout.reset(wgpuDeviceCreateBindGroupLayout(device, &desc));
  }
  const std::array<WGPUBindGroupLayout, 2> groups = {
      m_uiFrameLayout.get(), m_uiTextureLayout.get()};
  WGPUPipelineLayoutDescriptor layoutDesc = {};
  layoutDesc.bindGroupLayoutCount = groups.size();
  layoutDesc.bindGroupLayouts = groups.data();
  const wgpu::PipelineLayout layout(
      wgpuDeviceCreatePipelineLayout(device, &layoutDesc));
  const wgpu::ShaderModule module(
      wgpu::makeShader(device, std::string(shaders::uiWgsl).c_str()));

  // One instance a quad: its five vec4s.
  std::array<WGPUVertexAttribute, 5> attributes{};
  for (uint32_t i = 0; i < attributes.size(); ++i)
    attributes[i] = attribute(WGPUVertexFormat_Float32x4, i * 16u, i);
  WGPUVertexBufferLayout instances = {};
  instances.stepMode = WGPUVertexStepMode_Instance;
  instances.arrayStride = sizeof(UiInstance);
  instances.attributeCount = attributes.size();
  instances.attributes = attributes.data();

  // Over what's there, by alpha.
  WGPUBlendState blending = {};
  blending.color = {WGPUBlendOperation_Add,
                    WGPUBlendFactor_SrcAlpha,
                    WGPUBlendFactor_OneMinusSrcAlpha};
  blending.alpha = {WGPUBlendOperation_Add,
                    WGPUBlendFactor_One,
                    WGPUBlendFactor_OneMinusSrcAlpha};
  WGPUColorTargetState color = {};
  color.format = m_context.format();
  color.writeMask = WGPUColorWriteMask_All;
  color.blend = &blending;
  WGPUFragmentState fragment = {};
  fragment.module = module.get();
  fragment.entryPoint = wgpu::sv("fs_main");
  fragment.targetCount = 1;
  fragment.targets = &color;

  WGPURenderPipelineDescriptor desc = {};
  desc.layout = layout.get();
  desc.vertex.module = module.get();
  desc.vertex.entryPoint = wgpu::sv("vs_main");
  desc.vertex.bufferCount = 1;
  desc.vertex.buffers = &instances;
  desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  desc.primitive.cullMode = WGPUCullMode_None;
  desc.multisample.count = 1;
  desc.multisample.mask = 0xFFFFFFFFu;
  desc.fragment = &fragment;
  m_uiPipeline.reset(wgpuDeviceCreateRenderPipeline(device, &desc));

  m_uiUniforms = makeBuffer(device,
                            sizeof(glm::vec4),
                            WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
  {
    std::array<WGPUBindGroupEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].buffer = m_uiUniforms.get();
    entries[0].size = sizeof(glm::vec4);
    entries[1].binding = 1;
    entries[1].sampler = m_clampSampler.get();
    WGPUBindGroupDescriptor bindings = {};
    bindings.layout = m_uiFrameLayout.get();
    bindings.entryCount = entries.size();
    bindings.entries = entries.data();
    m_uiFrameBindings.reset(wgpuDeviceCreateBindGroup(device, &bindings));
  }

  // White, for boxes (which don't read it).
  GpuImage white = makeImage(device, {1, 1}, WGPUTextureFormat_RGBA8Unorm);
  const uint8_t pixel[4] = {255, 255, 255, 255};
  writeImage(m_context.queue(), white, {0, 0}, {1, 1}, pixel, 4);
  m_uiWhite.texture = std::move(white.texture);
  m_uiWhite.view = std::move(white.view);
  m_uiWhite.size = {1, 1};
  {
    WGPUBindGroupEntry entry = {};
    entry.binding = 0;
    entry.textureView = m_uiWhite.view.get();
    WGPUBindGroupDescriptor bindings = {};
    bindings.layout = m_uiTextureLayout.get();
    bindings.entryCount = 1;
    bindings.entries = &entry;
    m_uiWhite.bindings.reset(wgpuDeviceCreateBindGroup(device, &bindings));
  }
  return m_uiPipeline && m_uiFrameBindings && m_uiWhite.bindings;
}

void WebGpuRenderer::encodeUi()
{
  if (m_plan.uiInstances().empty())
    return;
  const WGPUDevice device = m_context.device();
  const WGPUQueue queue = m_context.queue();

  // Each texture drawn from, up to date on the GPU.
  for (auto& [id, texture] : m_uiTextures)
    texture.used = false;
  const auto gpuTexture = [&](const UiTexture* source) -> GpuUiTexture*
  {
    if (!source || source->size.x <= 0 || source->size.y <= 0 ||
        source->pixels.size() <
            std::size_t(source->size.x) * source->size.y * 4)
      return &m_uiWhite;
    GpuUiTexture& texture = m_uiTextures[source->id];
    if (texture.size != source->size)
    {
      GpuImage image =
          makeImage(device, source->size, WGPUTextureFormat_RGBA8Unorm);
      texture.texture = std::move(image.texture);
      texture.view = std::move(image.view);
      texture.size = source->size;
      texture.revision = source->revision + 1; // uploaded below
      WGPUBindGroupEntry entry = {};
      entry.binding = 0;
      entry.textureView = texture.view.get();
      WGPUBindGroupDescriptor bindings = {};
      bindings.layout = m_uiTextureLayout.get();
      bindings.entryCount = 1;
      bindings.entries = &entry;
      texture.bindings.reset(wgpuDeviceCreateBindGroup(device, &bindings));
    }
    if (texture.revision != source->revision)
    {
      writeTexture(queue,
                   texture.texture.get(),
                   {0, 0},
                   source->size,
                   source->pixels.data(),
                   4);
      texture.revision = source->revision;
    }
    texture.used = true;
    return &texture;
  };

  const std::vector<UiInstance>& instances = m_plan.uiInstances();

  if (instances.size() > m_uiCapacity)
  {
    m_uiCapacity = std::max(instances.size(), m_uiCapacity * 2);
    m_uiInstances =
        makeBuffer(device,
                   m_uiCapacity * sizeof(UiInstance),
                   WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst);
  }
  wgpuQueueWriteBuffer(queue,
                       m_uiInstances.get(),
                       0,
                       instances.data(),
                       instances.size() * sizeof(UiInstance));
  const WGPUTextureFormat format = m_context.format();
  const bool srgb = format == WGPUTextureFormat_BGRA8UnormSrgb ||
                    format == WGPUTextureFormat_RGBA8UnormSrgb;
  const glm::vec4 screen(glm::vec2(m_viewport), srgb ? 1.0f : 0.0f, 0.0f);
  wgpuQueueWriteBuffer(queue, m_uiUniforms.get(), 0, &screen, sizeof(screen));

  WGPURenderPassColorAttachment color = {};
  color.view = m_target;
  color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  color.loadOp = WGPULoadOp_Load;
  color.storeOp = WGPUStoreOp_Store;
  WGPURenderPassDescriptor desc = {};
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color;
  WGPURenderPassEncoder pass =
      wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
  wgpuRenderPassEncoderSetPipeline(pass, m_uiPipeline.get());
  wgpuRenderPassEncoderSetBindGroup(
      pass, 0, m_uiFrameBindings.get(), 0, nullptr);
  wgpuRenderPassEncoderSetVertexBuffer(
      pass, 0, m_uiInstances.get(), 0, instances.size() * sizeof(UiInstance));
  for (const FramePlan::UiBatch& batch : m_plan.uiBatches())
  {
    wgpuRenderPassEncoderSetBindGroup(
        pass, 1, gpuTexture(batch.texture)->bindings.get(), 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 6, batch.count, 0, batch.first);
  }
  endPass(pass);

  // Textures no longer drawn from.
  std::erase_if(
      m_uiTextures, [](const auto& entry) { return !entry.second.used; });
}

bool WebGpuRenderer::encodeDepthOfField()
{
  const std::optional<FocusUniforms> uniforms =
      m_plan.focusUniforms(m_viewport);
  if (!uniforms || !m_capabilities.depthOfField)
    return false;
  wgpuQueueWriteBuffer(
      m_context.queue(), m_focusParams.get(), 0, &*uniforms, sizeof(*uniforms));
  drawFullscreen(m_focusHalf.view.get(),
                 m_focusPreparePipeline.get(),
                 m_focusPrepareBindings.get(),
                 false);
  drawFullscreen(m_focusBlur.view.get(),
                 m_focusBlurPipeline.get(),
                 m_focusBlurBindings.get(),
                 false);
  drawFullscreen(m_focusOut.view.get(),
                 m_focusCombinePipeline.get(),
                 m_focusCombineBindings.get(),
                 false);
  return true;
}

void WebGpuRenderer::ensureInstanceCapacity(std::size_t instanceCount)
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

void WebGpuRenderer::uploadFrame()
{
  FrameUniforms uniforms = m_plan.frameUniforms();
  // A physical sky's reflections, once they're rendered.
  if (m_plan.frame().sky.physical && m_skyReady)
  {
    uniforms.skyMode.y = 1.0f;
    uniforms.skyMode.z = static_cast<float>(kSkyReflectionMips - 1);
  }
  // Ambient occlusion, if its passes run this frame.
  if (!m_occluded)
    uniforms.ambientOcclusion.x = 0.0f;
  wgpuQueueWriteBuffer(
      m_context.queue(), m_frameUniforms.get(), 0, &uniforms, sizeof(uniforms));

  const std::vector<MeshInstance>& instances = m_plan.instances();
  if (instances.empty())
    return;
  ensureInstanceCapacity(instances.size());
  wgpuQueueWriteBuffer(m_context.queue(),
                       m_instanceBuffer.get(),
                       0,
                       instances.data(),
                       instances.size() * sizeof(MeshInstance));
}

void WebGpuRenderer::encodeShadowPass()
{
  // Each map's view-projection: the one map's, or each cascade's.
  const FrameParams& frame = m_plan.frame();
  const uint32_t cascades =
      std::min(m_plan.shadowCascades(), m_capabilities.sunShadowCascades);
  std::vector<uint8_t> matrices(kFaceStride * cascades);
  for (uint32_t c = 0; c < cascades; ++c)
  {
    const glm::mat4& matrix = cascades > 1
                                  ? frame.cascades.cascades[c].viewProjection
                                  : frame.lightViewProjection;
    std::memcpy(matrices.data() + kFaceStride * c, &matrix, sizeof(matrix));
  }
  wgpuQueueWriteBuffer(m_context.queue(),
                       m_cascadeUniforms.get(),
                       0,
                       matrices.data(),
                       matrices.size());

  WGPURenderPassDepthStencilAttachment depth = {};
  depth.view = m_shadowView.get();
  depth.depthLoadOp = WGPULoadOp_Clear;
  depth.depthStoreOp = WGPUStoreOp_Store;
  depth.depthClearValue = 1.0f;

  WGPURenderPassDescriptor desc = {};
  desc.depthStencilAttachment = &depth;

  WGPURenderPassEncoder pass =
      wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
  m_boundPipeline = nullptr;
  if (!m_plan.instances().empty())
    wgpuRenderPassEncoderSetVertexBuffer(
        pass, 1, m_instanceBuffer.get(), 0, WGPU_WHOLE_SIZE);
  for (uint32_t c = 0; c < cascades; ++c)
  {
    // Cascades each draw into their tile of the atlas.
    if (cascades > 1)
    {
      const auto tile = static_cast<float>(m_shadowResolution);
      const float x = static_cast<float>(c % 2) * tile;
      const float y = static_cast<float>(c / 2) * tile;
      wgpuRenderPassEncoderSetViewport(pass, x, y, tile, tile, 0.0f, 1.0f);
      wgpuRenderPassEncoderSetScissorRect(pass,
                                          static_cast<uint32_t>(x),
                                          static_cast<uint32_t>(y),
                                          m_shadowResolution,
                                          m_shadowResolution);
    }
    const auto offset = static_cast<uint32_t>(kFaceStride * c);
    wgpuRenderPassEncoderSetBindGroup(
        pass, 0, m_shadowBindings.get(), 1, &offset);
    for (const Draw& draw :
         cascades > 1 ? m_plan.cascadeDraws(c) : m_plan.shadowDraws())
      encodeDraw(pass, draw, Sections::All);
  }
  endPass(pass);
}

void WebGpuRenderer::encodeLitPass()
{
  const glm::vec3& sky = m_plan.frame().lighting.skyColor;

  // The scene and the light that blooms, both cleared to the sky: drawn a
  // few samples a pixel, and resolved (averaged) into the scene's textures.
  std::array<WGPURenderPassColorAttachment, 2> colors{};
  for (WGPURenderPassColorAttachment& color : colors)
  {
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Discard; // the resolve keeps what's needed
    color.clearValue = WGPUColor{sky.r, sky.g, sky.b, 1.0};
  }
  if (m_sampleCount > 1)
  {
    colors[0].view = m_sceneSamplesView.get();
    colors[0].resolveTarget = m_sceneView.get();
    colors[1].view = m_glowSamplesView.get();
    colors[1].resolveTarget = m_glowSourceView.get();
  }
  else
  {
    // One sample: drawn straight into the scene's textures, kept.
    colors[0].view = m_sceneView.get();
    colors[1].view = m_glowSourceView.get();
    for (WGPURenderPassColorAttachment& color : colors)
      color.storeOp = WGPUStoreOp_Store;
  }

  WGPURenderPassDepthStencilAttachment depth = {};
  depth.view = m_depthView.get();
  depth.depthLoadOp = WGPULoadOp_Clear;
  // Kept for depth of field.
  depth.depthStoreOp = m_plan.frame().depthOfField.enabled
                           ? WGPUStoreOp_Store
                           : WGPUStoreOp_Discard;
  depth.depthClearValue = 1.0f;

  WGPURenderPassDescriptor desc = {};
  desc.colorAttachmentCount = colors.size();
  desc.colorAttachments = colors.data();
  desc.depthStencilAttachment = &depth;

  WGPURenderPassEncoder pass =
      wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
  if (!m_plan.instances().empty())
    wgpuRenderPassEncoderSetVertexBuffer(
        pass, 1, m_instanceBuffer.get(), 0, WGPU_WHOLE_SIZE);

  // The frame's bindings stay set across the surfaces' pipelines (they share
  // group 0's layout).
  m_boundPipeline = nullptr;
  wgpuRenderPassEncoderSetBindGroup(pass, 0, m_litBindings.get(), 0, nullptr);
  for (const Draw& draw : m_plan.litDraws())
    encodeDraw(pass, draw, Sections::Opaque);
  encodeGrass(pass);
  encodeSkyBackground(pass);

  // The see-through pipelines differ from the opaque ones.
  m_boundPipeline = nullptr;
  for (const Draw& draw : m_plan.blendDraws())
    encodeDraw(pass, draw, Sections::SeeThrough);
  endPass(pass);
}

void WebGpuRenderer::encodeDraw(WGPURenderPassEncoder pass,
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
    const bool blend = isSeeThrough(material.alphaMode);
    const bool opaque =
        which == Sections::Opaque || which == Sections::DepthNormals;
    if (section.indexCount == 0 || (opaque && blend) ||
        (which == Sections::SeeThrough && !blend && !draw.allSections))
      continue;
    const WGPURenderPipeline pipeline =
        which == Sections::DepthNormals
            ? depthNormalsPipeline(*material.surface)
            : pipelineFor(*material.surface, which, material.alphaMode);
    if (!pipeline)
      continue;
    if (pipeline != m_boundPipeline)
    {
      m_boundPipeline = pipeline;
      wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    }
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

WGPURenderPipeline WebGpuRenderer::pipelineFor(const Surface& surface,
                                               Sections which,
                                               AlphaMode mode)
{
  switch (which)
  {
  case Sections::All:
    return surface.shadow.get();
  case Sections::SeeThrough:
    return mode == AlphaMode::Additive ? surface.additive.get()
                                       : surface.blend.get();
  case Sections::LightShadow:
    return surface.pointShadow.get();
  case Sections::DepthNormals: // (see depthNormalsPipeline())
  case Sections::Opaque:
    break;
  }
  return surface.lit.get();
}

bool WebGpuRenderer::prepareOcclusion()
{
  if (!m_plan.frame().ambientOcclusion.enabled || m_occlusionBroken)
    return false;
  const WGPUDevice device = m_context.device();

  // The full-screen passes: the occlusion and its blur, reading a texture
  // texel by texel, with their parameters.
  if (!m_occlusionPipeline)
  {
    ValidationScope scope(device);
    std::array<WGPUBindGroupLayoutEntry, 2> entries{};
    entries[0] = wgpu::storageEntry(
        0, WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform);
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    WGPUBindGroupLayoutDescriptor layoutDesc = {};
    layoutDesc.entryCount = entries.size();
    layoutDesc.entries = entries.data();
    m_occlusionLayout.reset(
        wgpuDeviceCreateBindGroupLayout(device, &layoutDesc));
    const WGPUBindGroupLayout groups[] = {m_occlusionLayout.get()};
    WGPUPipelineLayoutDescriptor pipelineLayoutDesc = {};
    pipelineLayoutDesc.bindGroupLayoutCount = 1;
    pipelineLayoutDesc.bindGroupLayouts = groups;
    const wgpu::PipelineLayout layout(
        wgpuDeviceCreatePipelineLayout(device, &pipelineLayoutDesc));
    const wgpu::ShaderModule module(
        wgpu::makeShader(device, shaders::occlusionPasses().c_str()));
    const auto build = [&](const char* entry)
    {
      WGPUColorTargetState color = {};
      color.format = kSceneFormat;
      color.writeMask = WGPUColorWriteMask_All;
      WGPUFragmentState fragment = {};
      fragment.module = module.get();
      fragment.entryPoint = wgpu::sv(entry);
      fragment.targetCount = 1;
      fragment.targets = &color;
      WGPURenderPipelineDescriptor desc = {};
      desc.layout = layout.get();
      desc.vertex.module = module.get();
      desc.vertex.entryPoint = wgpu::sv("vs_main");
      desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
      desc.primitive.cullMode = WGPUCullMode_None;
      desc.multisample.count = 1;
      desc.multisample.mask = 0xFFFFFFFFu;
      desc.fragment = &fragment;
      return wgpu::RenderPipeline(
          wgpuDeviceCreateRenderPipeline(device, &desc));
    };
    m_occlusionPipeline = build("fs_occlusion");
    m_occlusionBlurPipeline = build("fs_blur");
    const std::optional<std::string> error = scope.end();
    if (error || !m_occlusionPipeline || !m_occlusionBlurPipeline)
    {
      LOG_ERROR("Ambient occlusion's passes don't build" +
                (error ? ": " + *error : std::string()));
      m_occlusionPipeline.reset();
      m_occlusionBlurPipeline.reset();
      m_occlusionBroken = true;
      return false;
    }
  }

  // Its targets at half the viewport's size (rounded up), and each pass's
  // bindings: the occlusion reads the normals, the blur across reads the
  // occlusion, the blur down what that blurred.
  const glm::ivec2 size = glm::max((m_viewport + 1) / 2, glm::ivec2(1));
  if (m_occlusion.size == size && m_occlusion.occlusionView)
    return true;
  const auto target = [&](wgpu::Texture& texture)
  {
    WGPUTextureDescriptor desc = {};
    desc.usage =
        WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = {
        static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), 1};
    desc.format = kSceneFormat;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    texture.reset(wgpuDeviceCreateTexture(device, &desc));
    return wgpu::TextureView(wgpuTextureCreateView(texture.get(), nullptr));
  };
  OcclusionTargets& targets = m_occlusion;
  targets.normalsView = target(targets.normals);
  targets.occlusionView = target(targets.occlusion);
  targets.blurredView = target(targets.blurred);
  targets.depthView = makeDepthTexture(
      device, size, WGPUTextureUsage_RenderAttachment, targets.depth);
  const std::array<WGPUTextureView, 3> sources = {targets.normalsView.get(),
                                                  targets.occlusionView.get(),
                                                  targets.blurredView.get()};
  for (std::size_t i = 0; i < sources.size(); ++i)
  {
    targets.params[i] =
        makeBuffer(device,
                   sizeof(OcclusionUniforms),
                   WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    std::array<WGPUBindGroupEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].buffer = targets.params[i].get();
    entries[0].size = sizeof(OcclusionUniforms);
    entries[1].binding = 1;
    entries[1].textureView = sources[i];
    WGPUBindGroupDescriptor desc = {};
    desc.layout = m_occlusionLayout.get();
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    targets.bindings[i].reset(wgpuDeviceCreateBindGroup(device, &desc));
  }
  targets.size = size;
  rebuildLitBindings();
  return true;
}

void WebGpuRenderer::encodeOcclusion()
{
  const glm::ivec2 size = m_occlusion.size;
  const std::array<glm::ivec2, 3> steps = {
      glm::ivec2(0, 0), glm::ivec2(1, 0), glm::ivec2(0, 1)};
  for (std::size_t i = 0; i < steps.size(); ++i)
  {
    const OcclusionUniforms uniforms = m_plan.occlusionUniforms(size, steps[i]);
    wgpuQueueWriteBuffer(m_context.queue(),
                         m_occlusion.params[i].get(),
                         0,
                         &uniforms,
                         sizeof(uniforms));
  }

  // What's drawn opaque: its normals and depth (0 where nothing is).
  WGPURenderPassColorAttachment color = {};
  color.view = m_occlusion.normalsView.get();
  color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  color.loadOp = WGPULoadOp_Clear;
  color.storeOp = WGPUStoreOp_Store;
  color.clearValue = WGPUColor{0.0, 0.0, 0.0, 0.0};
  WGPURenderPassDepthStencilAttachment depth = {};
  depth.view = m_occlusion.depthView.get();
  depth.depthLoadOp = WGPULoadOp_Clear;
  depth.depthStoreOp = WGPUStoreOp_Discard;
  depth.depthClearValue = 1.0f;
  WGPURenderPassDescriptor desc = {};
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color;
  desc.depthStencilAttachment = &depth;
  WGPURenderPassEncoder pass =
      wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
  if (!m_plan.instances().empty())
    wgpuRenderPassEncoderSetVertexBuffer(
        pass, 1, m_instanceBuffer.get(), 0, WGPU_WHOLE_SIZE);
  const uint32_t offset = 0;
  wgpuRenderPassEncoderSetBindGroup(
      pass, 0, m_shadowBindings.get(), 1, &offset);
  m_boundPipeline = nullptr;
  for (const Draw& draw : m_plan.litDraws())
    encodeDraw(pass, draw, Sections::DepthNormals);
  endPass(pass);

  // The occlusion, then blurred across and back down.
  drawFullscreen(m_occlusion.occlusionView.get(),
                 m_occlusionPipeline.get(),
                 m_occlusion.bindings[0].get(),
                 false);
  drawFullscreen(m_occlusion.blurredView.get(),
                 m_occlusionBlurPipeline.get(),
                 m_occlusion.bindings[1].get(),
                 false);
  drawFullscreen(m_occlusion.occlusionView.get(),
                 m_occlusionBlurPipeline.get(),
                 m_occlusion.bindings[2].get(),
                 false);
}

WGPURenderPipeline WebGpuRenderer::depthNormalsPipeline(const Surface& surface)
{
  if (surface.depthNormals || surface.depthNormalsBroken)
    return surface.depthNormals.get();
  const WGPUDevice device = m_context.device();
  ValidationScope scope(device);
  const wgpu::ShaderModule module(wgpu::makeShader(
      device,
      shaders::meshPass(surface.wgsl, shaders::meshDepthNormalsWgsl).c_str()));
  const std::array<WGPUBindGroupLayout, 2> groups = {
      m_shadowLayout.get(), surface.materialLayout.get()};
  WGPUPipelineLayoutDescriptor layoutDesc = {};
  layoutDesc.bindGroupLayoutCount = groups.size();
  layoutDesc.bindGroupLayouts = groups.data();
  const wgpu::PipelineLayout layout(
      wgpuDeviceCreatePipelineLayout(device, &layoutDesc));

  const MeshVertexLayouts layouts;
  const WGPUDepthStencilState depth =
      depthState(true, WGPUCompareFunction_Less);
  WGPUColorTargetState color = {};
  color.format = kSceneFormat;
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
  surface.depthNormals.reset(wgpuDeviceCreateRenderPipeline(device, &desc));
  const std::optional<std::string> error = scope.end();
  if (error || !surface.depthNormals)
  {
    LOG_ERROR("A material's depth for ambient occlusion doesn't build" +
              (error ? ": " + *error : std::string()));
    surface.depthNormals.reset();
    surface.depthNormalsBroken = true;
  }
  return surface.depthNormals.get();
}

const WebGpuRenderer::GpuMesh* WebGpuRenderer::findMesh(MeshHandle mesh) const
{
  if (!mesh.valid() || (mesh.id & kMeshSlotMask) == 0)
    return nullptr;
  const uint32_t index = meshSlotOf(mesh);
  if (index >= m_meshes.size() ||
      m_meshGenerations[index] != mesh.id >> kMeshSlotBits)
    return nullptr;

  const std::optional<GpuMesh>& slot = m_meshes[index];
  return slot ? &*slot : nullptr;
}

} // namespace sfs
