#pragma once

#include "engine/runtime/rendering3d/iRenderer3D.h"
#include "engine/webgpu/wgpuHandle.h"

#include <webgpu/webgpu.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace sfs
{

class WebGpuContext;

/**
 * IRenderer3D on WebGPU: a forward renderer with a sun shadow map.
 *
 * endFrame() records two passes into the backend's command encoder: the
 * shadow pass (depth-only, every shadow-casting instance from the sun's view)
 * then the lit pass (clears to the sky colour, draws every visible instance
 * with the shared lighting model). Instances of a mesh draw in one instanced
 * call per pass (per textured section in the lit pass); all of a frame's
 * instance data goes up in a single write.
 *
 * Textures upload once (with mipmaps) and are shared by every mesh that uses
 * the same TextureData, until the last of those meshes is destroyed.
 *
 * See-through things (Blend sections, and instances whose tint alpha is below
 * 1) draw after everything opaque, one instance at a time, back to front,
 * without writing depth. Masked and see-through sections cast shadows only
 * where they're solid enough (alpha at least the cutoff, or a half).
 *
 * Owned by WebGpuRenderBackend, which points it at each frame's encoder and
 * backbuffer with setTarget().
 */
class WebGpuRenderer3D final : public IRenderer3D
{
public:
  explicit WebGpuRenderer3D(WebGpuContext& context,
                            uint32_t shadowMapResolution = 2048);
  ~WebGpuRenderer3D() override;

  WebGpuRenderer3D(const WebGpuRenderer3D&) = delete;
  WebGpuRenderer3D& operator=(const WebGpuRenderer3D&) = delete;

  /**
   * Build pipelines and the shadow map. Call once before use.
   *
   * @return false (logged) if a GPU resource failed to build.
   */
  bool initialize();

  /**
   * Where this frame renders. A null encoder or view (e.g. the surface was
   * lost) makes endFrame() record nothing.
   */
  void setTarget(WGPUCommandEncoder encoder,
                 WGPUTextureView target,
                 glm::ivec2 size);

  MeshHandle createMesh(const MeshData& mesh) override;
  void destroyMesh(MeshHandle mesh) override;
  MaterialHandle createMaterial(const MeshMaterial& material) override;
  void updateMaterial(MaterialHandle handle, const MeshMaterial& material) override;
  void destroyMaterial(MaterialHandle material) override;
  glm::ivec2 viewportSize() const override { return m_viewport; }
  uint32_t shadowMapResolution() const override { return m_shadowResolution; }
  void beginFrame(const FrameParams3D& frame) override;
  void drawMesh(const MeshBatch3D& batch) override;
  void endFrame() override;

private:
  struct GpuTexture
  {
    wgpu::Texture texture;
    wgpu::TextureView view;
    bool nearest = false;
  };

  // A material on the GPU: its parameters, and the bind group (group 1 of
  // every mesh pipeline) with its textures.
  struct GpuMaterial
  {
    AlphaMode alphaMode = AlphaMode::Opaque;
    wgpu::Buffer uniforms;
    std::shared_ptr<GpuTexture> baseTexture;       // null: white
    std::shared_ptr<GpuTexture> metallicRoughness; // null: white
    wgpu::BindGroup bindings;
  };

  struct GpuSection
  {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    GpuMaterial material; // the mesh's own
  };

  struct GpuMesh
  {
    wgpu::Buffer vertices;
    wgpu::Buffer indices;
    uint32_t indexCount = 0;
    std::vector<GpuSection> sections;
  };

  // An uploaded texture, keyed by its TextureData's address. `source` tells a
  // live entry from one whose data was freed and the address reused.
  struct CachedTexture
  {
    std::weak_ptr<const TextureData> source;
    std::weak_ptr<GpuTexture> gpu;
  };

  // One instanced draw: a mesh, its run of instances in m_instances, and its
  // sections' materials in place of their own (a run of m_drawMaterials; 0:
  // the section's own).
  struct Draw
  {
    uint32_t meshSlot = 0;
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
    uint32_t firstMaterial = 0;
    uint32_t materialCount = 0;
    bool allSections = false; // see-through draws: every section, not just Blend ones
  };

  // A see-through draw, sorted back to front before drawing.
  struct BlendDraw
  {
    Draw draw;
    float distance = 0.0f; // from the camera
  };

  bool createPipelines();
  bool createTextureResources();
  std::shared_ptr<GpuTexture> uploadTexture(const std::shared_ptr<const TextureData>& texture);
  std::optional<GpuMaterial> makeMaterial(const MeshMaterial& material);
  // The material a draw uses for its mesh's section `index`.
  const GpuMaterial& materialFor(const GpuMesh& mesh, std::size_t index, const Draw& draw) const;
  void createShadowMap();
  void ensureDepthTarget();
  void ensureInstanceCapacity(std::size_t instanceCount);
  void uploadFrame();
  void encodeShadowPass();
  void encodeLitPass();
  // Which of a draw's sections to draw: All (the shadow pass), Opaque (all but
  // Blend ones) or SeeThrough (Blend ones, or all for an allSections draw).
  enum class Sections { All, Opaque, SeeThrough };
  void encodeDraw(WGPURenderPassEncoder pass, const Draw& draw, Sections which);

  const GpuMesh* findMesh(MeshHandle mesh) const;

  WebGpuContext& m_context;
  uint32_t m_shadowResolution;

  // Mesh slots; a handle's id is its slot index + 1 (0 stays the null handle).
  std::vector<std::optional<GpuMesh>> m_meshes;
  std::vector<uint32_t> m_freeSlots;

  // Objects' own materials (createMaterial), in slots like meshes.
  std::vector<std::optional<GpuMaterial>> m_materials;
  std::vector<uint32_t> m_freeMaterialSlots;

  wgpu::Buffer m_frameUniforms;
  wgpu::Buffer m_instanceBuffer;
  std::size_t m_instanceCapacity = 0;

  wgpu::Texture m_shadowMap;
  wgpu::TextureView m_shadowView;
  wgpu::Sampler m_shadowSampler;

  wgpu::Texture m_depth;
  wgpu::TextureView m_depthView;
  glm::ivec2 m_depthSize{0, 0};

  wgpu::BindGroupLayout m_shadowLayout;
  wgpu::BindGroupLayout m_litLayout;
  wgpu::BindGroupLayout m_materialLayout;
  wgpu::Sampler m_linearSampler;
  wgpu::Sampler m_nearestSampler;
  std::shared_ptr<GpuTexture> m_white; // for missing textures
  std::unordered_map<const TextureData*, CachedTexture> m_textures;
  wgpu::BindGroup m_shadowBindings;
  wgpu::BindGroup m_litBindings;
  wgpu::RenderPipeline m_shadowPipeline;
  wgpu::RenderPipeline m_litPipeline;
  wgpu::RenderPipeline m_blendPipeline;

  // This frame's target, set by the backend.
  WGPUCommandEncoder m_encoder = nullptr;
  WGPUTextureView m_target = nullptr;
  glm::ivec2 m_viewport{0, 0};

  // This frame's work, gathered between beginFrame() and endFrame().
  bool m_frameOpen = false;
  FrameParams3D m_frame;
  std::vector<PointLight3D> m_pointLights; // m_frame.pointLights copied out
  std::vector<MeshInstance> m_instances;
  std::vector<uint32_t> m_drawMaterials; // Draw::firstMaterial indexes this
  std::vector<Draw> m_litDraws;
  std::vector<BlendDraw> m_blendDraws;
  std::vector<Draw> m_shadowDraws;
};

} // namespace sfs
