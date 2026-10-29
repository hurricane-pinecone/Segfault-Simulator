#pragma once

#include "engine/core/rendering/3d/lightClusters.h"
#include "engine/runtime/rendering3d/iRenderer3D.h"
#include "engine/webgpu/wgpuHandle.h"

#include <webgpu/webgpu.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sfs
{

class WebGpuContext;

/**
 * Most point lights with shadows at once (ShadowSettings3D::pointLights is
 * capped to it): six cube faces each fit a texture array's 256 layers.
 */
inline constexpr uint32_t kMaxShadowedPointLights = 32;

namespace materialGraph
{
class MaterialShader;
}

/**
 * IRenderer3D on WebGPU: a forward renderer with a sun shadow map, clustered
 * point lights and bloom.
 *
 * endFrame() records its passes into the backend's command encoder: the
 * shadow pass (depth-only, every shadow-casting instance from the sun's view);
 * the lit pass (clears to the sky colour, draws every visible instance with
 * the shared lighting model into an HDR scene texture); bloom (a chain of
 * half-size textures, down then back up; see post3D.wgsl); and the composite,
 * which adds the bloom and writes the target. Instances of a mesh draw in one
 * instanced call per pass (per textured section in the lit pass); all of a
 * frame's instance data goes up in a single write.
 *
 * Point lights go up in a storage buffer, with each cluster's list of the
 * ones that reach it (see lightClusters.h), worked out on the CPU each frame:
 * a pixel only looks at its cluster's. Those casting shadows
 * (PointLight3D::castsShadow, up to kMaxShadowedPointLights) each get a cube
 * shadow map, a layer of one cube array, drawn before the lit pass from the
 * casters within their reach (MeshBatch3D::lightShadowInstances).
 *
 * Textures upload once (with mipmaps) and are shared by every mesh that uses
 * the same TextureData, until the last of those meshes is destroyed.
 *
 * A material's surface is the standard one, or a material graph's compiled
 * shader (MeshMaterial::shader). Each surface has its own pipelines, built the
 * first time a material uses it and shared by every material with the same
 * WGSL and alpha mode, so materials differing only in their values share
 * them. A graph whose shader fails to build (logged) draws magenta.
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
  void updateMaterial(MaterialHandle handle,
                      const MeshMaterial& material) override;
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

  // A surface's pipelines: the standard surface's, or a material graph's.
  // Each binds its materials as group 1, laid out by materialLayout.
  struct Surface
  {
    wgpu::BindGroupLayout materialLayout;
    wgpu::RenderPipeline shadow;
    wgpu::RenderPipeline lit;
    wgpu::RenderPipeline blend;       // see-through
    wgpu::RenderPipeline pointShadow; // a point light's shadow map's face
  };

  // A material on the GPU: its surface, its parameters, and the bind group
  // (group 1 of its surface's pipelines) with its textures.
  struct GpuMaterial
  {
    AlphaMode alphaMode = AlphaMode::Opaque;
    std::shared_ptr<const Surface> surface;
    wgpu::Buffer uniforms;
    std::vector<std::shared_ptr<GpuTexture>> textures; // the bind group's
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
    // A sphere around its vertices (for which point lights' reach it's in).
    glm::vec3 boundsCenter{0.0f};
    float boundsRadius = 0.0f;
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
    bool allSections =
        false; // see-through draws: every section, not just Blend ones
  };

  // A see-through draw, sorted back to front before drawing.
  struct BlendDraw
  {
    Draw draw;
    float distance = 0.0f; // from the camera
  };

  bool createPipelines();
  // Bloom's and the composite's (post3D.wgsl).
  bool createPostPipelines();
  bool createTextureResources();
  std::shared_ptr<GpuTexture>
  uploadTexture(const std::shared_ptr<const TextureData>& texture);
  // A 1x1 texture of one value (read as data), kept for reuse: what a graph's
  // texture reads without an image.
  std::shared_ptr<GpuTexture> solidTexture(const glm::vec4& value);
  // Build a surface's pipelines from its WGSL (see mesh3DSurface.wgsl); null
  // (logged) if they don't build.
  std::shared_ptr<Surface> buildSurface(const std::string& surfaceWgsl,
                                        wgpu::BindGroupLayout materialLayout);
  // A material graph shader's surface for an alpha mode: shared while any
  // material uses it.
  std::shared_ptr<const Surface>
  graphSurface(const materialGraph::MaterialShader& shader, AlphaMode mode);
  std::optional<GpuMaterial> makeMaterial(const MeshMaterial& material);
  std::optional<GpuMaterial> makeStandardMaterial(const MeshMaterial& material);
  std::optional<GpuMaterial> makeGraphMaterial(const MeshMaterial& material);
  // The material a draw uses for its mesh's section `index`.
  const GpuMaterial&
  materialFor(const GpuMesh& mesh, std::size_t index, const Draw& draw) const;
  void createShadowMap();
  // The depth, HDR scene and bloom textures, sized to the viewport.
  void ensureTargets();
  // Room in the light and cluster buffers (rebuilding the lit bindings when
  // they grow).
  void ensureLightBuffers(std::size_t lights,
                          std::size_t clusters,
                          std::size_t indices);
  void rebuildLitBindings();
  void uploadLights();
  // A post pass: one full-screen triangle into `target`, added onto what's
  // there with `blend`.
  void drawFullscreen(WGPUTextureView target,
                      WGPURenderPipeline pipeline,
                      WGPUBindGroup bindings,
                      bool blend);
  void encodeBloom();
  void encodeComposite();

  // Point-light shadows: the cube maps (room for `lights`, faces
  // `resolution` texels; rebuilding the lit bindings when remade), which
  // lights get them this frame and their casters, and drawing them.
  void ensurePointShadowMaps(uint32_t lights, uint32_t resolution);
  void assignShadowedLights();
  void encodePointShadows();
  void ensureInstanceCapacity(std::size_t instanceCount);
  void uploadFrame();
  void encodeShadowPass();
  void encodeLitPass();
  // Which of a draw's sections to draw: All (the shadow pass), Opaque (all but
  // Blend ones) or SeeThrough (Blend ones, or all for an allSections draw).
  enum class Sections
  {
    All, // the sun's shadow map
    Opaque,
    SeeThrough,
    LightShadow, // a point light's shadow map (every section)
  };
  void encodeDraw(WGPURenderPassEncoder pass, const Draw& draw, Sections which);
  // The pipeline a surface draws with in a pass (see Sections).
  static WGPURenderPipeline pipelineFor(const Surface& surface, Sections which);

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
  // The lit scene, HDR (emission and glints can go above 1).
  wgpu::Texture m_scene;
  wgpu::TextureView m_sceneView;
  // The light of it that blooms: the scene x each material's Bloom.
  wgpu::Texture m_glowSource;
  wgpu::TextureView m_glowSourceView;

  // Bloom: level 0 half the viewport, each next half again. A level's `down`
  // bindings read the one above (level 0's, the light that blooms); its `up`
  // ones read the one below, to add onto it.
  struct BloomLevel
  {
    glm::ivec2 size{0, 0};
    wgpu::Texture texture;
    wgpu::TextureView view;
    wgpu::Buffer downParams;
    wgpu::BindGroup down;
    wgpu::Buffer upParams;
    wgpu::BindGroup up;
  };
  std::vector<BloomLevel> m_bloom;
  wgpu::Buffer m_compositeParams;
  wgpu::BindGroup m_compositeBindings;
  wgpu::BindGroupLayout m_postLayout;
  wgpu::Sampler m_clampSampler;
  wgpu::RenderPipeline m_prefilterPipeline;
  wgpu::RenderPipeline m_downsamplePipeline;
  wgpu::RenderPipeline m_upsamplePipeline;
  wgpu::RenderPipeline m_compositePipeline;

  // Point lights and their clusters, on the GPU (capacities in elements).
  wgpu::Buffer m_lightBuffer;
  wgpu::Buffer m_clusterRangeBuffer;
  wgpu::Buffer m_clusterIndexBuffer;
  std::size_t m_lightCapacity = 0;
  std::size_t m_clusterRangeCapacity = 0;
  std::size_t m_clusterIndexCapacity = 0;
  ClusterGrid m_grid;
  ClusterLights m_clusters;

  // Point-light shadow maps: a cube array, a cube per shadowed light, and a
  // view of each face to draw into; the faces' uniforms (each at a 256-byte
  // offset) and the pass's bindings.
  wgpu::Texture m_pointShadowMap;
  wgpu::TextureView m_pointShadowCubes;
  std::vector<wgpu::TextureView> m_pointShadowFaces;
  uint32_t m_pointShadowCapacity = 0; // lights
  uint32_t m_pointShadowResolution = 0;
  wgpu::BindGroupLayout m_pointShadowLayout;
  wgpu::Buffer m_faceUniforms;
  wgpu::BindGroup m_pointShadowBindings;

  wgpu::BindGroupLayout m_shadowLayout;
  wgpu::BindGroupLayout m_litLayout;
  wgpu::Sampler m_linearSampler;
  wgpu::Sampler m_nearestSampler;
  std::shared_ptr<GpuTexture> m_white; // for missing textures
  std::unordered_map<uint32_t, std::shared_ptr<GpuTexture>>
      m_solidTextures; // by RGBA8 value
  std::unordered_map<const TextureData*, CachedTexture> m_textures;
  wgpu::BindGroup m_shadowBindings;
  wgpu::BindGroup m_litBindings;
  std::shared_ptr<const Surface> m_standard;
  // Material graph surfaces by WGSL and alpha mode, while materials use them.
  std::unordered_map<std::string, std::weak_ptr<const Surface>> m_surfaces;
  // Ones that didn't build (already logged), so they aren't tried again.
  std::unordered_set<std::string> m_brokenSurfaces;
  // The surface whose pipeline the pass being encoded has set.
  const Surface* m_boundSurface = nullptr;

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
  // Casters for the shadowed point lights, as drawMesh() got them: each
  // batch's draw and its run of m_casters.
  struct CasterBatch
  {
    Draw base;
    std::size_t first = 0;
    std::size_t count = 0;
  };
  std::vector<CasterBatch> m_casterBatches;
  std::vector<MeshInstance> m_casters;
  // The frame's shadowed lights (indices into m_pointLights; the i-th has
  // cube i) and the draws of the casters within each one's reach.
  struct ShadowedLight
  {
    std::size_t light = 0;
    std::vector<Draw> draws;
  };
  std::vector<ShadowedLight> m_shadowedLights;
};

} // namespace sfs
