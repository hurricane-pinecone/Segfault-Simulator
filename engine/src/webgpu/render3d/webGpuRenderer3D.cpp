#include "engine/webgpu/render3d/webGpuRenderer3D.h"

#include "engine/core/logger/logger.h"
#include "engine/core/rendering/3d/frustum.h"
#include "engine/core/rendering/3d/materialGraphCompiler.h"
#include "engine/webgpu/webGpuContext.h"
#include "engine/webgpu/wgpuUtil.h"
#include "glm/glm/ext/matrix_clip_space.hpp"
#include "glm/glm/ext/scalar_constants.hpp"
#include "glm/glm/ext/vector_int4.hpp"
#include "glm/glm/ext/vector_uint4.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/gtc/constants.hpp"
#include "glm/glm/matrix.hpp"
#include "glm/glm/trigonometric.hpp"

#include <engine/generated/embeddedWgsl.h>

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
constexpr uint32_t kSceneSamples = 4;
// Bloom's levels: up to this many, each half the one before, while they're
// at least kBloomSmallest pixels.
constexpr int kBloomLevels = 6;
constexpr int kBloomSmallest = 8;

// Mirrors `Frame` in mesh3DCommon.wgsl (uniform layout: 16-byte aligned).
struct FrameUniforms
{
  glm::mat4 viewProjection;
  glm::mat4 lightViewProjection;
  glm::vec4 cameraPosition;
  glm::vec4 sunDirection;
  glm::vec4 sunColor;
  glm::vec4 shadow;
  glm::vec4 sky;
  glm::vec4 time;          // x seconds
  glm::vec4 cameraForward; // xyz
  glm::uvec4 clusterSize;  // tiles across, down, slices; lights
  glm::vec4
      clusterDepth; // near, far, slices per log(depth / near), tile pixels
  glm::vec4 wind;   // which way it blows (unit xy), strength, gustiness
  glm::vec4 haze;   // strength, 1 / its half distance
};

static_assert(sizeof(FrameUniforms) == 64 * 2 + 16 * 11,
              "FrameUniforms must match the WGSL Frame layout");

// Mirrors `PointLight` in mesh3DLit.wgsl (a storage buffer's elements).
struct GpuPointLight
{
  glm::vec4 positionRadius;
  glm::vec4 colorIntensity;
  glm::vec4 shadow; // its cube's layer (-1: none), 1 / its face size, its size
};

// Mirrors `ShadowFace` in mesh3DPointShadow.wgsl; each face's sits at a
// multiple of kFaceStride in one buffer (a dynamic offset's alignment).
struct FaceUniforms
{
  glm::mat4 viewProjection;
  glm::vec4 light; // position, 1 / reach
};
constexpr uint64_t kFaceStride = 256;
static_assert(sizeof(FaceUniforms) <= kFaceStride);

// A view from a point light into cube face `face` (+X, -X, +Y, -Y, +Z, -Z, as
// a cube texture's layers go), laid out as the GPU samples cube maps: a face's
// texel (s, t) looks along its axis with s across it and t down it as the
// spec's face table has them, so drawing it with this view puts each
// direction's surface where sampling in that direction reads.
glm::mat4 cubeFaceView(int face, const glm::vec3& light)
{
  // Rows: view x, y and z (looking down -z) from the offset from the light.
  static const glm::vec3 kRows[6][3] = {
      {{0, 0, -1}, {0, 1, 0}, {-1, 0, 0}}, // +X: s = -z, t = -y
      {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}},   // -X: s = +z, t = -y
      {{1, 0, 0}, {0, 0, -1}, {0, -1, 0}}, // +Y: s = +x, t = +z
      {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}},   // -Y: s = +x, t = -z
      {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}},  // +Z: s = +x, t = -y
      {{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  // -Z: s = -x, t = -y
  };
  glm::mat4 view(1.0f);
  for (int row = 0; row < 3; ++row)
  {
    for (int column = 0; column < 3; ++column)
      view[column][row] = kRows[face][row][column];
    view[3][row] = -glm::dot(kRows[face][row], light);
  }
  return view;
}

// Mirrors `Grass` in mesh3DGrass.wgsl.
struct GrassUniforms
{
  glm::vec4 trample; // the trample map's world min, 1 / its world size
  glm::vec4 blade;   // stiffness, draw distance, how much the ground's tinted,
                     // full height
  glm::vec4 maps;    // the maps' grid min, 1 / its size (0: unpainted)
  glm::mat4 model;
  glm::mat4 normalMatrix;
  glm::mat4 inverseModel;
};

// Mirrors `Grow` in mesh3DGrassGrow.wgsl.
struct GrassGrowUniforms
{
  glm::mat4 model;
  glm::mat4 normalMatrix;
  std::array<glm::vec4, 6> planes;
  glm::vec4 camera; // xyz, draw distance
  glm::vec4 ground; // the ground grid's min, texel size
  glm::ivec4 groundSize;
  glm::vec4 maps;      // the maps' grid min, 1 / its size (0: unpainted)
  glm::vec4 shape;     // height, height variation, width, finest cell
  glm::vec4 reach;     // each lattice's
  glm::vec4 detail;    // 5 segments to, 2 to
  glm::uvec4 capacity; // each list's
  std::array<glm::ivec4, 4> lattices; // first cell, count
};
static_assert(sizeof(GrassGrowUniforms) == 416,
              "GrassGrowUniforms must match mesh3DGrassGrow.wgsl's Grow");

// Mirrors `QuadIn` in ui3D.wgsl: a UiQuad as the GPU reads it.
struct UiInstance
{
  glm::vec4 rect;
  glm::vec4 uv;
  glm::vec4 color;
  glm::vec4 borderColor;
  glm::vec4 shape; // radius, softness, border, kind
};

// Mirrors `Focus` in post3DFocus.wgsl.
struct FocusUniforms
{
  glm::vec4 depth; // projection[2][2], projection[3][2], perspective (1/0)
  glm::vec4 focus; // distance, half the sharp range, blur radius (texels)
  glm::vec4 texel; // a half-size texel (uv)
};

// Mirrors `Post` in post3D.wgsl.
struct PostUniforms
{
  glm::vec4 texel; // xy the source's texel size
  glm::vec4 bloom; // threshold, softness, radius, intensity
};
static_assert(sizeof(MeshVertex) == 44,
              "vertex layout below assumes 11 floats");
static_assert(sizeof(MeshInstance) == 80,
              "instance layout below assumes mat4 + vec4");

// Mirrors `Material` in mesh3DCommon.wgsl.
struct MaterialUniforms
{
  glm::vec4 baseColor;
  glm::vec4 params;      // normal scale, metallic, roughness, alpha cutoff
  glm::vec4 uvTransform; // tiling xy, offset zw
  glm::uvec4 mode;       // x: AlphaMode
};

// A pass's whole shader: the shared declarations, the surface interface, a
// material's surface, then the pass itself.
std::string passShader(const std::string& surface, std::string_view pass)
{
  std::string source(shaders::mesh3DCommonWgsl);
  source += shaders::mesh3DSurfaceWgsl;
  source += surface;
  source += pass;
  return source;
}

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

const char* alphaModeConstant(AlphaMode mode)
{
  switch (mode)
  {
  case AlphaMode::Mask:
    return "ALPHA_MASK";
  case AlphaMode::Blend:
    return "ALPHA_BLEND";
  case AlphaMode::Opaque:
    break;
  }
  return "ALPHA_OPAQUE";
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

namespace
{

// How a grass field's grown, by distance (fractions of its draw distance):
// each lattice's reach (see mesh3DGrassGrow.wgsl), and to where blades have
// 5 segments, then 2 (then 1).
constexpr std::array<float, 4> kGrassReach = {0.2f, 0.4f, 0.65f, 1.0f};
constexpr float kGrassFineDetail = 0.12f;
constexpr float kGrassMidDetail = 0.35f;
// Most blades a field grows a frame (its lists all together), and the most
// cells a side of a lattice.
constexpr double kGrassMostBlades = 4.0e6;
constexpr int kGrassMostCells = 2048;
constexpr uint32_t kGrownBladeBytes = 32;
// How much grass's colour covers the ground it grows on, where it's full.
constexpr float kGrassGroundTint = 0.9f;

} // namespace

WebGpuRenderer3D::GpuImage WebGpuRenderer3D::makeImage(WGPUDevice device,
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

void WebGpuRenderer3D::writeImage(WGPUQueue queue,
                                  const GpuImage& image,
                                  glm::ivec2 origin,
                                  glm::ivec2 size,
                                  const void* pixels,
                                  uint32_t bytesPerPixel)
{
  writeTexture(queue, image.texture.get(), origin, size, pixels, bytesPerPixel);
}

void WebGpuRenderer3D::writeTexture(WGPUQueue queue,
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

GrassHandle WebGpuRenderer3D::createGrass(const GrassGround& ground)
{
  if (ground.empty())
    return {};
  const WGPUDevice device = m_context.device();
  const WGPUQueue queue = m_context.queue();

  GpuGrass grass;
  grass.groundGrid = ground.grid();
  const glm::vec2 texel = ground.grid().texelSize();
  grass.groundArea =
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
  grass.capacity = {64, 64, 64};
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

void WebGpuRenderer3D::rebuildGrassBindings(GpuGrass& grass)
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
    entries[0].size = uint64_t(grass.capacity[list]) * kGrownBladeBytes;
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
    first += grass.capacity[list];
  }
}

void WebGpuRenderer3D::updateGrassMaps(GrassHandle handle,
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
    grass.mapsGrid = {};
    return;
  }

  const glm::ivec2 from =
      glm::clamp(image.from, glm::ivec2(0), image.grid.size);
  const glm::ivec2 to = glm::clamp(image.to, from, image.grid.size);
  const std::size_t bytes = std::size_t(to.x - from.x) * (to.y - from.y) * 4;
  if (image.shape.size() < bytes || image.data.size() < bytes)
    return;
  if (grass.mapsGrid.size != image.grid.size ||
      grass.shape.size != image.grid.size)
  {
    grass.shape =
        makeImage(device, image.grid.size, WGPUTextureFormat_RGBA8Unorm);
    grass.data =
        makeImage(device, image.grid.size, WGPUTextureFormat_RGBA8Unorm);
    rebuildGrassBindings(grass);
  }
  grass.mapsGrid = image.grid;
  writeImage(queue, grass.shape, from, to - from, image.shape.data(), 4);
  writeImage(queue, grass.data, from, to - from, image.data.data(), 4);
}

void WebGpuRenderer3D::updateGrassTrample(GrassHandle handle,
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
  grass.trampleBounds = glm::vec4(image.min, 1.0f / size);
}

void WebGpuRenderer3D::destroyGrass(GrassHandle grass)
{
  if (!grass.valid() || grass.id > m_grass.size() || !m_grass[grass.id - 1])
    return;
  m_grass[grass.id - 1].reset();
  m_freeGrassSlots.push_back(grass.id - 1);
}

void WebGpuRenderer3D::drawGrass(const GrassBatch3D& batch)
{
  if (m_frameOpen && batch.grass.valid() && batch.grass.id <= m_grass.size() &&
      m_grass[batch.grass.id - 1])
    m_grassDraws.push_back(batch);
}

WGPURenderPipeline WebGpuRenderer3D::grassPipeline(const Surface& surface,
                                                   bool ground)
{
  wgpu::RenderPipeline& built = ground ? surface.grassGround : surface.grass;
  bool& broken = ground ? surface.grassGroundBroken : surface.grassBroken;
  if (built || broken)
    return built.get();
  const WGPUDevice device = m_context.device();
  ValidationScope scope(device);

  const wgpu::ShaderModule module(
      wgpu::makeShader(device,
                       passShader(surface.wgsl,
                                  std::string(shaders::mesh3DLitWgsl) +
                                      std::string(shaders::mesh3DGrassWgsl))
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
  desc.multisample.count = kSceneSamples;
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

void WebGpuRenderer3D::growGrass()
{
  for (std::optional<GpuGrass>& grass : m_grass)
    if (grass)
      grass->grown = grass->placed = false;
  if (m_grassDraws.empty() || !m_grassGrowPipeline)
    return;
  const WGPUQueue queue = m_context.queue();
  const Frustum frustum = Frustum::fromViewProjection(m_frame.viewProjection);

  WGPUComputePassDescriptor passDesc = {};
  const WGPUComputePassEncoder pass =
      wgpuCommandEncoderBeginComputePass(m_encoder, &passDesc);
  wgpuComputePassEncoderSetPipeline(pass, m_grassGrowPipeline.get());
  for (const GrassBatch3D& batch : m_grassDraws)
  {
    GpuGrass& grass = *m_grass[batch.grass.id - 1];
    const GrassShape& shape = batch.shape;
    if (grass.placed || !(shape.density > 0.0f) || !(shape.height > 0.0f))
      continue;
    const glm::mat4& model = batch.model;
    const glm::vec3 acrossX(model[0]), acrossY(model[1]);
    // World area per square unit of its own XY, and the least a unit across
    // it is in the world.
    const float area =
        std::max(glm::length(glm::cross(acrossX, acrossY)), 1e-8f);
    const float least =
        std::max(std::min(glm::length(acrossX), glm::length(acrossY)), 1e-6f);
    const float density = std::min(shape.density, 20000.0f);
    const float cell = 1.0f / std::sqrt(density * area);
    const float distance = std::max(batch.drawDistance, 1.0f);
    const glm::mat4 normalMatrix(
        glm::transpose(glm::inverse(glm::mat3(model))));
    const glm::vec4 maps =
        grass.mapsGrid.empty()
            ? glm::vec4(0.0f)
            : glm::vec4(grass.mapsGrid.min,
                        1.0f / (grass.mapsGrid.max - grass.mapsGrid.min));

    // How it's drawn (its ground, even with no blades in reach).
    const GrassUniforms drawUniforms{
        grass.trampleBounds,
        glm::vec4(std::clamp(batch.stiffness, 0.0f, 1.0f),
                  distance,
                  kGrassGroundTint,
                  shape.height),
        maps,
        model,
        normalMatrix,
        glm::inverse(model)};
    wgpuQueueWriteBuffer(queue,
                         grass.drawUniforms.get(),
                         0,
                         &drawUniforms,
                         sizeof(drawUniforms));
    grass.placed = true;

    GrassGrowUniforms uniforms{};
    uniforms.model = model;
    uniforms.normalMatrix = normalMatrix;
    uniforms.planes = frustum.planes();
    uniforms.camera = glm::vec4(m_frame.cameraPosition, distance);
    const GrassGrid& ground = grass.groundGrid;
    uniforms.ground = glm::vec4(ground.min, ground.texelSize());
    uniforms.groundSize = glm::ivec4(ground.size, 0, 0);
    uniforms.maps = maps;
    uniforms.shape =
        glm::vec4(shape.height, shape.heightVariation, shape.width, cell);
    uniforms.detail = glm::vec4(
        distance * kGrassFineDetail, distance * kGrassMidDetail, 0.0f, 0.0f);

    // Each lattice's cells within its reach of the camera, on the ground.
    const glm::vec2 camera(glm::inverse(model) *
                           glm::vec4(m_frame.cameraPosition, 1.0f));
    glm::ivec2 most(0);
    for (int k = 0; k < 4; ++k)
    {
      const float reach = distance * kGrassReach[k];
      uniforms.reach[k] = reach;
      const float size = cell * static_cast<float>(1 << k);
      const float own = reach / least;
      const glm::vec2 low = glm::max(camera - own, ground.min);
      const glm::vec2 high = glm::min(camera + own, ground.max);
      glm::ivec2 first(glm::floor((low - ground.min) / size));
      glm::ivec2 count =
          glm::max(glm::ivec2(glm::ceil((high - ground.min) / size)) - first,
                   glm::ivec2(0));
      // Too many: those nearest the camera.
      for (int axis = 0; axis < 2; ++axis)
        if (count[axis] > kGrassMostCells)
        {
          first[axis] = static_cast<int>(std::floor(
                            (camera[axis] - ground.min[axis]) / size)) -
                        kGrassMostCells / 2;
          count[axis] = kGrassMostCells;
        }
      uniforms.lattices[k] = glm::ivec4(first, count);
      most = glm::max(most, count);
    }
    if (most.x <= 0 || most.y <= 0)
      continue;

    // Room for each list: about how many blades grow within its distances
    // (all round), but no more than the whole field has.
    const auto within = [&](float radius)
    {
      double count = 0.0;
      for (int k = 0; k < 4; ++k)
      {
        const double reach = std::min(radius, uniforms.reach[k]);
        count += density * (k < 3 ? 0.75 : 1.0) / double(1 << (2 * k)) *
                 glm::pi<double>() * reach * reach;
      }
      return count;
    };
    const double whole = double(grass.groundArea) * area * density;
    const std::array<double, 4> bands = {0.0,
                                         within(uniforms.detail.x),
                                         within(uniforms.detail.y),
                                         within(distance)};
    std::array<double, 3> needed{};
    double total = 0.0;
    for (int list = 0; list < 3; ++list)
    {
      needed[list] =
          std::min(bands[list + 1] - bands[list], whole) * 1.15 + 256.0;
      total += needed[list];
    }
    const double fit = std::min(1.0, kGrassMostBlades / total);
    bool grow = false;
    for (int list = 0; list < 3; ++list)
    {
      needed[list] *= fit;
      grow = grow || needed[list] > grass.capacity[list];
    }
    if (grow)
    {
      // Room to spare, so it isn't made again as the camera moves a little;
      // a multiple of 8 blades a list, for the lists' bindings' offsets.
      uint64_t blades = 0;
      for (int list = 0; list < 3; ++list)
      {
        const double room =
            std::max(needed[list] * 1.25, double(grass.capacity[list]));
        grass.capacity[list] = static_cast<uint32_t>(
            (static_cast<uint64_t>(std::min(room, kGrassMostBlades)) + 7) / 8 *
            8);
        blades += grass.capacity[list];
      }
      grass.blades = makeBuffer(m_context.device(),
                                blades * kGrownBladeBytes,
                                WGPUBufferUsage_Storage);
      if (!grass.blades)
        continue;
      rebuildGrassBindings(grass);
    }
    uniforms.capacity =
        glm::uvec4(grass.capacity[0], grass.capacity[1], grass.capacity[2], 0);

    // Each list's draw: its blades' strip (2 vertices a segment, and the tip),
    // as many as grow.
    const std::array<uint32_t, 12> draws = {
        11, 0, 0, 0, 5, 0, 0, 0, 3, 0, 0, 0};
    wgpuQueueWriteBuffer(
        queue, grass.draws.get(), 0, draws.data(), sizeof(draws));
    wgpuQueueWriteBuffer(
        queue, grass.growUniforms.get(), 0, &uniforms, sizeof(uniforms));

    wgpuComputePassEncoderSetBindGroup(
        pass, 0, grass.growBindings.get(), 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass,
        static_cast<uint32_t>(most.x + 7) / 8,
        static_cast<uint32_t>(most.y + 7) / 8,
        4);
    grass.grown = true;
  }
  wgpuComputePassEncoderEnd(pass);
  wgpuComputePassEncoderRelease(pass);
}

void WebGpuRenderer3D::encodeGrass(WGPURenderPassEncoder pass)
{
  for (const GrassBatch3D& batch : m_grassDraws)
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
    m_boundSurface = nullptr; // the mesh pipelines need setting again after

    // The ground first, tinted where grass covers it.
    const uint32_t meshSlot = batch.mesh.id - 1;
    const GpuMesh* mesh =
        batch.mesh.id > 0 && meshSlot < m_meshes.size() && m_meshes[meshSlot]
            ? &*m_meshes[meshSlot]
            : nullptr;
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
  m_grassDraws.clear();
  m_uiQuads.clear();
  m_casterBatches.clear();
  m_casters.clear();
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

  if (!batch.lightShadowInstances.empty())
  {
    m_casterBatches.push_back(
        {base, m_casters.size(), batch.lightShadowInstances.size()});
    m_casters.insert(m_casters.end(),
                     batch.lightShadowInstances.begin(),
                     batch.lightShadowInstances.end());
  }

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

  ensureTargets();
  assignShadowedLights(); // adds instances, so before they go up
  uploadFrame();
  uploadLights();

  if (m_frame.shadows.enabled)
    encodeShadowPass();
  encodePointShadows();
  growGrass();
  encodeLitPass();
  m_focused = encodeDepthOfField();
  if (m_frame.bloom.enabled && !m_bloom.empty())
    encodeBloom();
  encodeComposite();
  encodeUi();
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
  return m_linearSampler && m_nearestSampler && m_white;
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

std::shared_ptr<WebGpuRenderer3D::GpuTexture>
WebGpuRenderer3D::solidTexture(const glm::vec4& value)
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

std::optional<WebGpuRenderer3D::GpuMaterial>
WebGpuRenderer3D::makeMaterial(const MeshMaterial& material)
{
  if (!material.shader)
    return makeStandardMaterial(material);
  if (std::optional<GpuMaterial> gpu = makeGraphMaterial(material))
    return gpu;
  // A shader that doesn't build draws magenta, so it's noticed.
  MeshMaterial broken;
  broken.baseColor = {1.0f, 0.0f, 1.0f, 1.0f};
  return makeStandardMaterial(broken);
}

std::optional<WebGpuRenderer3D::GpuMaterial>
WebGpuRenderer3D::makeStandardMaterial(const MeshMaterial& material)
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

  const MaterialUniforms uniforms{
      material.baseColor,
      glm::vec4(material.normalScale,
                material.metallic,
                material.roughness,
                material.alphaCutoff),
      glm::vec4(material.tiling, material.offset),
      glm::uvec4(static_cast<uint32_t>(material.alphaMode), 0, 0, 0)};
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

std::optional<WebGpuRenderer3D::GpuMaterial>
WebGpuRenderer3D::makeGraphMaterial(const MeshMaterial& material)
{
  const materialGraph::MaterialShader& shader = *material.shader;
  const WGPUDevice device = m_context.device();
  GpuMaterial gpu;
  gpu.alphaMode = material.alphaMode;
  gpu.surface = graphSurface(shader, material.alphaMode);
  if (!gpu.surface)
    return std::nullopt;

  // The shader's parameters (at least one), in its order; missing ones 0.
  std::vector<glm::vec4> parameters(
      std::max<std::size_t>(shader.parameterCount(), 1), glm::vec4(0.0f));
  std::copy_n(material.shaderParameters.begin(),
              std::min(material.shaderParameters.size(), parameters.size()),
              parameters.begin());
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
    // Then the point lights, each cluster's range of light indices, and the
    // indices.
    // And the point lights' cube shadow maps.
    std::array<WGPUBindGroupLayoutEntry, 7> entries{};
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

    WGPUBindGroupLayoutDescriptor desc = {};
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    m_litLayout.reset(wgpuDeviceCreateBindGroupLayout(device, &desc));
  }

  // Bind groups. The shadow pass's is fixed for the renderer's lifetime (the
  // uniform buffer never changes); the lit pass's is rebuilt when the light
  // buffers grow.
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
        device, std::string(shaders::mesh3DGrassGrowWgsl).c_str()));
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
      std::string(shaders::mesh3DStandardSurfaceWgsl),
      wgpu::BindGroupLayout(wgpuDeviceCreateBindGroupLayout(device, &layout)));

  return m_shadowLayout && m_litLayout && m_shadowBindings && m_litBindings &&
         m_standard;
}

std::shared_ptr<WebGpuRenderer3D::Surface>
WebGpuRenderer3D::buildSurface(const std::string& surfaceWgsl,
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
        device, passShader(surfaceWgsl, shaders::mesh3DShadowWgsl).c_str()));
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
        passShader(surfaceWgsl, shaders::mesh3DPointShadowWgsl).c_str()));
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
        passShader(surfaceWgsl,
                   std::string(shaders::mesh3DLitWgsl) +
                       std::string(shaders::mesh3DLitVertexWgsl))
            .c_str()));
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
      desc.multisample.count = kSceneSamples;
      desc.multisample.mask = 0xFFFFFFFFu;
      desc.fragment = &fragment;
      return wgpu::RenderPipeline(
          wgpuDeviceCreateRenderPipeline(device, &desc));
    };
    surface->lit = build(false);
    surface->blend = build(true);
  }

  if (const std::optional<std::string> error = scope.end())
  {
    LOG_ERROR("A material's shader doesn't build: " + *error);
    return nullptr;
  }
  if (!surface->shadow || !surface->lit || !surface->blend ||
      !surface->pointShadow)
    return nullptr;
  surface->materialLayout = std::move(materialLayout);
  return surface;
}

std::shared_ptr<const WebGpuRenderer3D::Surface>
WebGpuRenderer3D::graphSurface(const materialGraph::MaterialShader& shader,
                               AlphaMode mode)
{
  // The alpha mode is baked in, so the passes' branches on it fold away.
  const std::string wgsl = shader.wgsl() +
                           "\nfn surfaceAlphaMode() -> u32 {\n  return " +
                           alphaModeConstant(mode) + ";\n}\n";
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

void WebGpuRenderer3D::ensureTargets()
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
      kSceneSamples);
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
    desc.sampleCount = kSceneSamples;
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
  m_focusPrepareBindings = focusBindings(m_sceneView.get(), black->view.get());
  m_focusBlurBindings =
      focusBindings(m_focusHalf.view.get(), black->view.get());
  m_focusCombineBindings =
      focusBindings(m_sceneView.get(), m_focusBlur.view.get());
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

void WebGpuRenderer3D::ensureLightBuffers(std::size_t lights,
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

void WebGpuRenderer3D::rebuildLitBindings()
{
  // Not until the light buffers and shadow maps are all there.
  if (!m_lightBuffer || !m_clusterRangeBuffer || !m_clusterIndexBuffer ||
      !m_pointShadowCubes)
    return;
  std::array<WGPUBindGroupEntry, 7> entries{};
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

  WGPUBindGroupDescriptor desc = {};
  desc.layout = m_litLayout.get();
  desc.entryCount = entries.size();
  desc.entries = entries.data();
  m_litBindings.reset(wgpuDeviceCreateBindGroup(m_context.device(), &desc));
}

void WebGpuRenderer3D::ensurePointShadowMaps(uint32_t lights,
                                             uint32_t resolution)
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

void WebGpuRenderer3D::assignShadowedLights()
{
  // The lights casting shadows, up to the budget, each with the casters
  // within its reach (a caster's bounding sphere, placed, touching the
  // light's), copied into the frame's instances.
  m_shadowedLights.clear();
  const std::size_t count = std::min(m_pointLights.size(), kMaxPointLights3D);
  const std::size_t budget = std::min<std::size_t>(
      m_frame.shadows.pointLights, kMaxShadowedPointLights);
  for (std::size_t i = 0; i < count && m_shadowedLights.size() < budget; ++i)
    if (m_pointLights[i].castsShadow)
      m_shadowedLights.push_back({i, {}});
  if (m_shadowedLights.empty())
    return;
  ensurePointShadowMaps(static_cast<uint32_t>(m_shadowedLights.size()),
                        m_frame.shadows.pointResolution);

  for (ShadowedLight& shadowed : m_shadowedLights)
  {
    const PointLight3D& light = m_pointLights[shadowed.light];
    for (const CasterBatch& batch : m_casterBatches)
    {
      const std::optional<GpuMesh>& mesh = m_meshes[batch.base.meshSlot];
      if (!mesh)
        continue;
      Draw draw = batch.base;
      draw.firstInstance = static_cast<uint32_t>(m_instances.size());
      for (std::size_t k = 0; k < batch.count; ++k)
      {
        const MeshInstance& instance = m_casters[batch.first + k];
        const glm::vec3 center(instance.model *
                               glm::vec4(mesh->boundsCenter, 1.0f));
        const float scale =
            std::max({glm::length(glm::vec3(instance.model[0])),
                      glm::length(glm::vec3(instance.model[1])),
                      glm::length(glm::vec3(instance.model[2]))});
        if (glm::distance(center, light.position) >
            light.radius + mesh->boundsRadius * scale)
          continue;
        m_instances.push_back(instance);
        ++draw.instanceCount;
      }
      if (draw.instanceCount > 0)
        shadowed.draws.push_back(draw);
    }
  }
}

void WebGpuRenderer3D::encodePointShadows()
{
  if (m_shadowedLights.empty())
    return;

  // Every face's view, in one write.
  std::vector<uint8_t> faces(kFaceStride * m_shadowedLights.size() * 6);
  for (std::size_t i = 0; i < m_shadowedLights.size(); ++i)
  {
    const PointLight3D& light = m_pointLights[m_shadowedLights[i].light];
    const float reach = std::max(light.radius, 1e-3f);
    const float near = std::clamp(light.shadowNear, 0.01f, reach * 0.5f);
    const glm::mat4 projection =
        glm::perspectiveRH_ZO(glm::half_pi<float>(), 1.0f, near, reach);
    for (int face = 0; face < 6; ++face)
    {
      const FaceUniforms uniforms{
          projection * cubeFaceView(face, light.position),
          glm::vec4(light.position, 1.0f / reach)};
      std::memcpy(faces.data() + kFaceStride * (i * 6 + face),
                  &uniforms,
                  sizeof(uniforms));
    }
  }
  wgpuQueueWriteBuffer(
      m_context.queue(), m_faceUniforms.get(), 0, faces.data(), faces.size());

  for (std::size_t i = 0; i < m_shadowedLights.size(); ++i)
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
      m_boundSurface = nullptr;
      const auto offset = static_cast<uint32_t>(kFaceStride * (i * 6 + face));
      wgpuRenderPassEncoderSetBindGroup(
          pass, 0, m_pointShadowBindings.get(), 1, &offset);
      if (!m_shadowedLights[i].draws.empty())
        wgpuRenderPassEncoderSetVertexBuffer(
            pass, 1, m_instanceBuffer.get(), 0, WGPU_WHOLE_SIZE);
      for (const Draw& draw : m_shadowedLights[i].draws)
        encodeDraw(pass, draw, Sections::LightShadow);
      endPass(pass);
    }
}

void WebGpuRenderer3D::uploadLights()
{
  // The lights (nearest the camera's target first, as the render system
  // keeps them), and each cluster's list of the ones that reach it.
  const std::size_t count = std::min(m_pointLights.size(), kMaxPointLights3D);
  const std::span<const PointLight3D> lights(m_pointLights.data(), count);
  ClusterView view;
  view.viewProjection = m_frame.viewProjection;
  view.viewport = glm::vec2(m_viewport);
  view.cameraPosition = m_frame.cameraPosition;
  view.forward = m_frame.cameraForward;
  assignLights(m_grid, view, lights, m_clusters);

  ensureLightBuffers(
      count, m_clusters.ranges.size(), m_clusters.indices.size());
  std::vector<GpuPointLight> gpu(count);
  const float texel =
      1.0f / static_cast<float>(std::max(m_pointShadowResolution, 1u));
  for (std::size_t i = 0; i < count; ++i)
    gpu[i] = {glm::vec4(lights[i].position, lights[i].radius),
              glm::vec4(lights[i].color, lights[i].intensity),
              glm::vec4(-1.0f, texel, lights[i].sourceRadius, 0.0f)};
  for (std::size_t i = 0; i < m_shadowedLights.size(); ++i)
    gpu[m_shadowedLights[i].light].shadow.x = static_cast<float>(i);
  const WGPUQueue queue = m_context.queue();
  if (!gpu.empty())
    wgpuQueueWriteBuffer(queue,
                         m_lightBuffer.get(),
                         0,
                         gpu.data(),
                         gpu.size() * sizeof(GpuPointLight));
  wgpuQueueWriteBuffer(queue,
                       m_clusterRangeBuffer.get(),
                       0,
                       m_clusters.ranges.data(),
                       m_clusters.ranges.size() * sizeof(glm::uvec2));
  if (!m_clusters.indices.empty())
    wgpuQueueWriteBuffer(queue,
                         m_clusterIndexBuffer.get(),
                         0,
                         m_clusters.indices.data(),
                         m_clusters.indices.size() * sizeof(uint32_t));
}

bool WebGpuRenderer3D::createPostPipelines()
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
      wgpu::makeShader(device, std::string(shaders::post3DWgsl).c_str()));

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
  // to mix in.
  {
    std::array<WGPUBindGroupLayoutEntry, 5> focus{};
    focus[0] = entries[0];
    focus[1] = entries[1];
    focus[2] = entries[2];
    focus[3].binding = 3;
    focus[3].visibility = WGPUShaderStage_Fragment;
    focus[3].texture.sampleType = WGPUTextureSampleType_Depth;
    focus[3].texture.viewDimension = WGPUTextureViewDimension_2D;
    focus[3].texture.multisampled = kSceneSamples > 1;
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
    const wgpu::ShaderModule focusModule(wgpu::makeShader(
        device, std::string(shaders::post3DFocusWgsl).c_str()));
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
         m_focusLayout && m_focusPreparePipeline && m_focusBlurPipeline &&
         m_focusCombinePipeline;
}

void WebGpuRenderer3D::drawFullscreen(WGPUTextureView target,
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

void WebGpuRenderer3D::encodeBloom()
{
  const BloomSettings3D& bloom = m_frame.bloom;
  const glm::vec4 settings(std::max(bloom.threshold, 0.0f),
                           std::max(bloom.softness, 0.0f),
                           std::max(bloom.radius, 0.0f),
                           std::max(bloom.intensity, 0.0f));
  const auto write = [&](const wgpu::Buffer& buffer, glm::ivec2 sourceSize)
  {
    const PostUniforms uniforms{
        glm::vec4(1.0f / glm::vec2(sourceSize), 0.0f, 0.0f), settings};
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

void WebGpuRenderer3D::encodeComposite()
{
  // The scene plus the glow (none when bloom is off), into the target.
  const BloomSettings3D& bloom = m_frame.bloom;
  const PostUniforms uniforms{
      glm::vec4(0.0f),
      glm::vec4(0.0f,
                0.0f,
                0.0f,
                bloom.enabled ? std::max(bloom.intensity, 0.0f) : 0.0f)};
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

void WebGpuRenderer3D::drawUi(const UiDrawList& list)
{
  if (m_frameOpen)
    m_uiQuads.insert(m_uiQuads.end(), list.quads.begin(), list.quads.end());
}

bool WebGpuRenderer3D::createUiPipeline()
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
      wgpu::makeShader(device, std::string(shaders::ui3DWgsl).c_str()));

  // One instance a quad: its five vec4s.
  std::array<WGPUVertexAttribute, 5> attributes{};
  for (uint32_t i = 0; i < attributes.size(); ++i)
    attributes[i] = {WGPUVertexFormat_Float32x4, i * 16u, i};
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

void WebGpuRenderer3D::encodeUi()
{
  if (m_uiQuads.empty())
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

  // The quads as instances, in batches sharing a texture.
  struct Batch
  {
    GpuUiTexture* texture;
    uint32_t first;
    uint32_t count;
  };
  std::vector<UiInstance> instances;
  std::vector<Batch> batches;
  instances.reserve(m_uiQuads.size());
  for (const UiQuad& quad : m_uiQuads)
  {
    GpuUiTexture* texture =
        quad.kind == UiQuad::Box ? &m_uiWhite : gpuTexture(quad.texture);
    // Boxes bind white but needn't break a run of glyphs: they don't read it.
    if (batches.empty() ||
        (batches.back().texture != texture && quad.kind != UiQuad::Box))
      batches.push_back({texture, static_cast<uint32_t>(instances.size()), 0});
    ++batches.back().count;
    instances.push_back({quad.rect,
                         quad.uv,
                         quad.color,
                         quad.borderColor,
                         glm::vec4(quad.radius,
                                   quad.softness,
                                   quad.border,
                                   static_cast<float>(quad.kind))});
  }
  m_uiQuads.clear();

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
  for (const Batch& batch : batches)
  {
    wgpuRenderPassEncoderSetBindGroup(
        pass, 1, batch.texture->bindings.get(), 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 6, batch.count, 0, batch.first);
  }
  endPass(pass);

  // Textures no longer drawn from.
  std::erase_if(
      m_uiTextures, [](const auto& entry) { return !entry.second.used; });
}

bool WebGpuRenderer3D::encodeDepthOfField()
{
  const DepthOfFieldSettings3D& settings = m_frame.depthOfField;
  if (!settings.enabled || !(settings.blur > 0.0f))
    return false;
  // Depth back to distance, by the projection (perspective or not); the blur
  // as wide on any screen, about 16 pixels a side at 1080p.
  const glm::mat4& projection = m_frame.projection;
  const glm::ivec2 half = glm::max(m_viewport / 2, glm::ivec2(1));
  const FocusUniforms uniforms{
      glm::vec4(projection[2][2],
                projection[3][2],
                projection[2][3] != 0.0f ? 1.0f : 0.0f,
                0.0f),
      glm::vec4(
          std::max(m_frame.focusDistance, 0.01f),
          std::max(settings.range, 0.0f) * 0.5f,
          settings.blur * 8.0f * static_cast<float>(m_viewport.y) / 1080.0f,
          0.0f),
      glm::vec4(1.0f / glm::vec2(half), 0.0f, 0.0f)};
  wgpuQueueWriteBuffer(
      m_context.queue(), m_focusParams.get(), 0, &uniforms, sizeof(uniforms));
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
  uniforms.time = glm::vec4(m_frame.time, 0.0f, 0.0f, 0.0f);
  uniforms.cameraForward = glm::vec4(m_frame.cameraForward, 0.0f);
  const float windAngle = glm::radians(m_frame.wind.direction);
  uniforms.wind = glm::vec4(std::cos(windAngle),
                            std::sin(windAngle),
                            std::max(m_frame.wind.strength, 0.0f),
                            std::max(m_frame.wind.gustiness, 0.0f));
  uniforms.haze = glm::vec4(std::clamp(m_frame.haze.strength, 0.0f, 1.0f),
                            1.0f / std::max(m_frame.haze.distance, 1e-3f),
                            0.0f,
                            0.0f);

  // The clusters the point lights are sorted into (uploadLights()).
  m_grid = ClusterGrid::forViewport(
      glm::vec2(m_viewport), m_frame.nearPlane, m_frame.farPlane);
  uniforms.clusterSize = glm::uvec4(
      m_grid.size,
      static_cast<uint32_t>(std::min(m_pointLights.size(), kMaxPointLights3D)));
  uniforms.clusterDepth =
      glm::vec4(m_grid.nearPlane,
                m_grid.farPlane,
                static_cast<float>(m_grid.size.z) /
                    std::log(m_grid.farPlane / m_grid.nearPlane),
                m_grid.tilePixels);

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
  m_boundSurface = nullptr;
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
  colors[0].view = m_sceneSamplesView.get();
  colors[0].resolveTarget = m_sceneView.get();
  colors[1].view = m_glowSamplesView.get();
  colors[1].resolveTarget = m_glowSourceView.get();

  WGPURenderPassDepthStencilAttachment depth = {};
  depth.view = m_depthView.get();
  depth.depthLoadOp = WGPULoadOp_Clear;
  // Kept for depth of field.
  depth.depthStoreOp =
      m_frame.depthOfField.enabled ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
  depth.depthClearValue = 1.0f;

  WGPURenderPassDescriptor desc = {};
  desc.colorAttachmentCount = colors.size();
  desc.colorAttachments = colors.data();
  desc.depthStencilAttachment = &depth;

  WGPURenderPassEncoder pass =
      wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
  if (!m_instances.empty())
    wgpuRenderPassEncoderSetVertexBuffer(
        pass, 1, m_instanceBuffer.get(), 0, WGPU_WHOLE_SIZE);

  // The frame's bindings stay set across the surfaces' pipelines (they share
  // group 0's layout).
  m_boundSurface = nullptr;
  wgpuRenderPassEncoderSetBindGroup(pass, 0, m_litBindings.get(), 0, nullptr);
  for (const Draw& draw : m_litDraws)
    encodeDraw(pass, draw, Sections::Opaque);
  encodeGrass(pass);

  // The see-through pipelines differ from the opaque ones.
  m_boundSurface = nullptr;
  for (const BlendDraw& blend : m_blendDraws)
    encodeDraw(pass, blend.draw, Sections::SeeThrough);
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
    if (material.surface.get() != m_boundSurface)
    {
      m_boundSurface = material.surface.get();
      wgpuRenderPassEncoderSetPipeline(
          pass, pipelineFor(*m_boundSurface, which));
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

WGPURenderPipeline WebGpuRenderer3D::pipelineFor(const Surface& surface,
                                                 Sections which)
{
  switch (which)
  {
  case Sections::All:
    return surface.shadow.get();
  case Sections::SeeThrough:
    return surface.blend.get();
  case Sections::LightShadow:
    return surface.pointShadow.get();
  case Sections::Opaque:
    break;
  }
  return surface.lit.get();
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
