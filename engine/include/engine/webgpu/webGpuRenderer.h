#pragma once

#include "engine/runtime/rendering/framePlan.h"
#include "engine/runtime/rendering/iRenderer.h"
#include "engine/webgpu/wgpuHandle.h"

#include <webgpu/webgpu.h>

#include <array>
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

namespace materialGraph
{
class MaterialShader;
}

/**
 * IRenderer on WebGPU: a forward renderer with a sun shadow map, clustered
 * point lights and bloom.
 *
 * endFrame() records its passes into the backend's command encoder: the
 * shadow pass (depth-only, every shadow-casting instance from the sun's view);
 * the lit pass (clears to the sky colour, draws every visible instance with
 * the shared lighting model into an HDR scene texture); bloom (a chain of
 * half-size textures, down then back up; see post.wgsl); and the composite,
 * which adds the bloom and writes the target. Instances of a mesh draw in one
 * instanced call per pass (per textured section in the lit pass); all of a
 * frame's instance data goes up in a single write.
 *
 * Point lights go up in a storage buffer, with each cluster's list of the
 * ones that reach it (see lightClusters.h), worked out on the CPU each frame:
 * a pixel only looks at its cluster's. Those casting shadows
 * (PointLight::castsShadow, up to kMaxShadowedPointLights) each get a cube
 * shadow map, a layer of one cube array, drawn before the lit pass from the
 * casters within their reach (MeshBatch::lightShadowInstances).
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
 * What draws where is worked out by a FramePlan, as for every backend; this
 * uploads what it works out and records the passes.
 *
 * Owned by WebGpuRenderBackend, which points it at each frame's encoder and
 * backbuffer with setTarget().
 */
class WebGpuRenderer final : public IRenderer
{
public:
  explicit WebGpuRenderer(WebGpuContext& context,
                          uint32_t shadowMapResolution = 2048);
  ~WebGpuRenderer() override;

  WebGpuRenderer(const WebGpuRenderer&) = delete;
  WebGpuRenderer& operator=(const WebGpuRenderer&) = delete;

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
  // Whether a handle is one of its meshes now (not destroyed, nor stale).
  bool hasMesh(MeshHandle mesh) const { return findMesh(mesh) != nullptr; }
  MaterialHandle createMaterial(const MeshMaterial& material) override;
  void updateMaterial(MaterialHandle handle,
                      const MeshMaterial& material) override;
  void destroyMaterial(MaterialHandle material) override;
  void updateTexture(const std::shared_ptr<const TextureData>& texture,
                     glm::ivec2 from,
                     glm::ivec2 to) override;
  RendererCapabilities capabilities() const override { return m_capabilities; }
  glm::ivec2 viewportSize() const override { return m_viewport; }
  uint32_t shadowMapResolution() const override { return m_shadowResolution; }
  void beginFrame(const FrameParams& frame) override;
  void drawMesh(const MeshBatch& batch) override;

  GrassHandle createGrass(const GrassGround& ground) override;
  void updateGrassGround(GrassHandle grass, const GrassGround& ground) override;
  void updateGrassMaps(GrassHandle grass, const GrassMapsImage& image) override;
  void updateGrassTrample(GrassHandle grass,
                          const GrassTrampleImage& image) override;
  void destroyGrass(GrassHandle grass) override;
  void drawGrass(const GrassBatch& batch) override;
  void drawUi(const UiDrawList& list) override;
  void endFrame() override;

private:
  struct GpuTexture
  {
    wgpu::Texture texture;
    wgpu::TextureView view;
    bool nearest = false;
    uint32_t levels = 1;   // mip levels
    glm::uvec2 size{0, 0}; // as uploaded (smaller if it had to be)
  };

  // A surface's pipelines: the standard surface's, or a material graph's.
  // Each binds its materials as group 1, laid out by materialLayout.
  struct Surface
  {
    wgpu::BindGroupLayout materialLayout;
    wgpu::RenderPipeline shadow;
    wgpu::RenderPipeline lit;
    wgpu::RenderPipeline blend;       // see-through
    wgpu::RenderPipeline additive;    // light added to what's behind
    wgpu::RenderPipeline pointShadow; // a point light's shadow map's face
    // Grass with this surface's look: built the first time grass uses it
    // (most surfaces never are), from its WGSL.
    std::string wgsl;
    mutable wgpu::RenderPipeline grass;
    mutable bool grassBroken = false;
    mutable wgpu::RenderPipeline grassGround; // the ground under grass
    mutable bool grassGroundBroken = false;
    // Its depth and normals, for ambient occlusion: built the first time a
    // frame has it.
    mutable wgpu::RenderPipeline depthNormals;
    mutable bool depthNormalsBroken = false;
  };

  // A texture and its view.
  struct GpuImage
  {
    wgpu::Texture texture;
    wgpu::TextureView view;
    glm::ivec2 size{0, 0};
  };

  // A grass field on the GPU: its ground and maps, where it's flattened, the
  // blades grown each frame (three lists, by detail, and the indices of those
  // casting the sun's shadow) with their indirect draws, the grow pass's
  // bindings, group 2 of the grass pipelines (one a list) and group 1 of its
  // shadow's.
  struct GpuGrass
  {
    GrassFieldState state; // its grids, trample bounds and blade capacity
    GpuImage ground;
    GpuImage shape;
    GpuImage data;
    GpuImage trample;

    wgpu::Buffer blades;
    wgpu::Buffer draws;
    wgpu::Buffer growUniforms;
    wgpu::Buffer drawUniforms;
    wgpu::Buffer casters;
    wgpu::BindGroup growBindings;
    std::array<wgpu::BindGroup, 3> drawBindings;
    wgpu::BindGroup shadowBindings;
    bool grown = false;    // this frame: its blades
    bool placed = false;   // this frame: its draw's uniforms (for its ground)
    bool casts = false;    // this frame: its blades into the sun's shadow
    uint32_t castInto = 0; // the cascades they're drawn into so far (bits)
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

  using Draw = FramePlan::Draw;

  // What the plan needs of a mesh.
  static PlannedMesh plannedMesh(const GpuMesh& mesh)
  {
    return {mesh.sections.size(), mesh.boundsCenter, mesh.boundsRadius};
  }

  bool createPipelines();
  // Bloom's and the composite's (post.wgsl).
  bool createPostPipelines();
  bool createTextureResources();
  // Without `mipmaps`, one level (a splat map, which updateTexture() can
  // change); the first upload of a texture decides.
  std::shared_ptr<GpuTexture>
  uploadTexture(const std::shared_ptr<const TextureData>& texture,
                bool mipmaps = true);
  // A 1x1 texture of one value (read as data), kept for reuse: what a graph's
  // texture reads without an image.
  std::shared_ptr<GpuTexture> solidTexture(const glm::vec4& value);
  // Build a surface's pipelines from its WGSL (see meshSurface.wgsl); null
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
  std::optional<GpuMaterial> makeTerrainMaterial(const MeshMaterial& material);
  // A terrain's layers as one texture array: their colours (sRGB, height in
  // alpha), or their detail (normal xy, roughness), each layer resized to the
  // largest; shared by terrains with the same textures.
  std::shared_ptr<GpuTexture> terrainLayers(const TerrainMaterial& terrain,
                                            bool detail);
  // The material a draw uses for its mesh's section `index`.
  const GpuMaterial&
  materialFor(const GpuMesh& mesh, std::size_t index, const Draw& draw) const;
  void createShadowMap();
  // The sun's shadow map sized for `cascades` (1: the one map; more: an
  // atlas of them), remade (and the lit bindings with it) when that changes.
  void ensureShadowAtlas(uint32_t cascades);
  // A physical sky's resources: its transmittance table, the sky and its
  // reflections, the BRDF table, and the passes that render them.
  bool createSkyResources();
  // Render a physical sky's reflections again if it changed (its revision).
  void encodeSky();
  void encodeSkyBackground(WGPURenderPassEncoder pass);
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
  // Depth of field, if the frame has it: whether it ran.
  bool encodeDepthOfField();
  // Grass: its bindings (after a texture or the blades change), the
  // pipeline a surface draws it with (null if it doesn't build), growing the
  // frame's blades (a compute pass, before the lit pass), and drawing them.
  void rebuildGrassBindings(GpuGrass& grass);
  // An RGBA texture to sample (and fill from the CPU); pixels into a
  // rectangle of one, from `origin`, `size` of them.
  static GpuImage
  makeImage(WGPUDevice device, glm::ivec2 size, WGPUTextureFormat format);
  static void writeImage(WGPUQueue queue,
                         const GpuImage& image,
                         glm::ivec2 origin,
                         glm::ivec2 size,
                         const void* pixels,
                         uint32_t bytesPerPixel);
  static void writeTexture(WGPUQueue queue,
                           WGPUTexture texture,
                           glm::ivec2 origin,
                           glm::ivec2 size,
                           const void* pixels,
                           uint32_t bytesPerPixel);
  WGPURenderPipeline grassPipeline(const Surface& surface, bool ground);
  // Ambient occlusion (and contact shadows, worked out with it): whether this
  // frame has it (its passes and targets made, the lit bindings pointed at
  // its map), the passes working it out, and a surface's depth-and-normals
  // pipeline (null if it doesn't build).
  bool prepareOcclusion();
  void encodeOcclusion();
  WGPURenderPipeline depthNormalsPipeline(const Surface& surface);
  void growGrass();
  void encodeGrass(WGPURenderPassEncoder pass);
  // The frame's grass into the sun's shadow map being drawn (group 0 bound at
  // `offset`, the map's).
  void encodeGrassShadows(WGPURenderPassEncoder pass,
                          uint32_t offset,
                          uint32_t cascade);
  void encodeComposite();
  // UI (drawUi), over the target after the composite.
  bool createUiPipeline();
  void encodeUi();

  // Point-light shadows: the cube maps (room for `lights`, faces
  // `resolution` texels; rebuilding the lit bindings when remade), which
  // lights get them this frame and their casters, and drawing them.
  void ensurePointShadowMaps(uint32_t lights, uint32_t resolution);
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
    LightShadow,  // a point light's shadow map (every section)
    DepthNormals, // ambient occlusion's first pass (as Opaque)
  };
  void encodeDraw(WGPURenderPassEncoder pass, const Draw& draw, Sections which);
  // The pipeline a surface draws with in a pass (see Sections).
  static WGPURenderPipeline
  pipelineFor(const Surface& surface, Sections which, AlphaMode mode);

  const GpuMesh* findMesh(MeshHandle mesh) const;

  // A mesh handle is its slot (plus one) in the low bits and the slot's
  // generation above, so a handle kept after its mesh is destroyed doesn't
  // find the mesh that takes the slot next (it draws nothing). The top bit
  // stays clear: MeshLibrary's stand-ins use it.
  static constexpr uint32_t kMeshSlotBits = 20;
  static constexpr uint32_t kMeshSlotMask = (1u << kMeshSlotBits) - 1;
  static constexpr uint32_t kMeshGenerations = 1u << (31 - kMeshSlotBits);
  // The slot a handle names (whether or not it's current).
  static uint32_t meshSlotOf(MeshHandle mesh)
  {
    return (mesh.id & kMeshSlotMask) - 1;
  }

  WebGpuContext& m_context;
  uint32_t m_shadowResolution;
  RendererCapabilities m_capabilities;
  // Samples a pixel the lit pass draws with (multisampled anti-aliasing),
  // resolved into the scene's textures after it.
  uint32_t m_sampleCount = 4;

  // Mesh slots; a handle's id is its slot index + 1 (0 stays the null handle).
  std::vector<std::optional<GpuMesh>> m_meshes;
  std::vector<uint32_t> m_meshGenerations; // per slot (see meshSlotOf())
  std::vector<uint32_t> m_freeSlots;

  // Objects' own materials (createMaterial), in slots like meshes.
  std::vector<std::optional<GpuMaterial>> m_materials;
  std::vector<uint32_t> m_freeMaterialSlots;
  std::vector<std::optional<GpuGrass>> m_grass;
  std::vector<uint32_t> m_freeGrassSlots;
  wgpu::BindGroupLayout m_grassLayout;
  wgpu::BindGroupLayout m_grassGrowLayout;
  wgpu::ComputePipeline m_grassGrowPipeline;
  wgpu::BindGroupLayout m_grassShadowLayout;
  wgpu::RenderPipeline m_grassShadowPipeline;

  // Timing the frame's main passes on the GPU, for measuring what they cost:
  // with SFS_GPU_TIMING set, on a device with timestamp queries, their
  // averages are logged every few seconds. Each pass writes a timestamp as it
  // begins and ends; every few frames they're copied out and read back.
  enum TimedPass : uint32_t
  {
    TimedSunShadows,
    TimedGrassGrowth,
    TimedLit,
    kTimedPasses,
  };
  struct GpuTiming
  {
    enum class Readback
    {
      Idle,
      Copied,  // this frame's copied out, to map once it's submitted
      Mapping, // waiting on the map
      Mapped,
    };
    wgpu::QuerySet queries;
    wgpu::Buffer resolved;
    wgpu::Buffer readback;
    Readback state = Readback::Idle;
    std::array<bool, kTimedPasses> ran{};     // this frame
    std::array<bool, kTimedPasses> copied{};  // the frame read back
    std::array<double, kTimedPasses> total{}; // milliseconds
    std::array<uint32_t, kTimedPasses> samples{};
    uint32_t readbacks = 0;
    WGPURenderPassTimestampWrites renderWrites{};
    WGPUComputePassTimestampWrites computeWrites{};
  };
  std::unique_ptr<GpuTiming> m_timing;
  // A pass's timestamp writes (null when not timing); the frame's read back
  // (at its start), and copied out (at its end).
  const WGPURenderPassTimestampWrites* timedRenderPass(TimedPass pass);
  const WGPUComputePassTimestampWrites* timedComputePass(TimedPass pass);
  void readTiming();
  void copyTiming();

  wgpu::Buffer m_frameUniforms;
  wgpu::Buffer m_instanceBuffer;
  std::size_t m_instanceCapacity = 0;

  wgpu::Texture m_shadowMap;
  wgpu::TextureView m_shadowView;
  wgpu::Sampler m_shadowSampler;
  glm::uvec2 m_shadowMapSize{0, 0}; // the atlas's, with cascades
  // Each sun shadow map's (cascade's) view-projection, 256 bytes apart (a
  // dynamic offset's alignment), for the shadow pass.
  wgpu::Buffer m_cascadeUniforms;

  // A physical sky (sky.wgsl): its transmittance table (from the CPU's
  // PhysicalSky), the sky rendered octahedrally (the background and haze
  // read it), its reflections (mip 0 sharp to the last, roughest), and the
  // split-sum BRDF table. Always there (small), so the lit pass's bindings
  // are the same with a flat sky.
  struct SkyTarget
  {
    wgpu::Texture texture;
    wgpu::TextureView view;              // every mip
    std::vector<wgpu::TextureView> mips; // each to draw into
  };
  wgpu::Texture m_skyTable;
  wgpu::TextureView m_skyTableView;
  SkyTarget m_skyRadiance;
  SkyTarget m_skyReflections;
  wgpu::Texture m_brdfTable;
  wgpu::TextureView m_brdfTableView;
  wgpu::Sampler m_skySampler;
  wgpu::BindGroupLayout m_skyLayout;
  wgpu::Buffer m_skyUniforms;
  std::vector<wgpu::Buffer> m_skyPassUniforms; // the radiance's, each mip's
  std::vector<wgpu::BindGroup> m_skyPassBindings;
  wgpu::RenderPipeline m_skyRadiancePipeline;
  wgpu::RenderPipeline m_skyPrefilterPipeline;
  wgpu::RenderPipeline m_skyBackgroundPipeline;
  std::shared_ptr<const PhysicalSky> m_skyModel; // whose table is uploaded
  uint64_t m_skyRevision = 0;                    // what the reflections show
  bool m_skyReady = false;

  wgpu::Texture m_depth;
  wgpu::TextureView m_depthView;
  glm::ivec2 m_depthSize{0, 0};
  // The lit scene, HDR (emission and glints can go above 1).
  wgpu::Texture m_scene;
  wgpu::TextureView m_sceneView;
  // The light of it that blooms: the scene x each material's Bloom.
  wgpu::Texture m_glowSource;
  wgpu::TextureView m_glowSourceView;
  // The lit pass's multisampled targets, resolved into the two above.
  wgpu::Texture m_sceneSamples;
  wgpu::TextureView m_sceneSamplesView;
  wgpu::Texture m_glowSamples;
  wgpu::TextureView m_glowSamplesView;

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

  // UI: this frame's quads, and the textures they draw from, by UiTexture id
  // (uploaded again when their revision changes; dropped once unused).
  struct GpuUiTexture
  {
    wgpu::Texture texture;
    wgpu::TextureView view;
    glm::ivec2 size{0, 0};
    uint32_t revision = 0;
    wgpu::BindGroup bindings;
    bool used = false; // this frame
  };
  std::unordered_map<uint32_t, GpuUiTexture> m_uiTextures;
  GpuUiTexture m_uiWhite; // what untextured boxes bind
  wgpu::RenderPipeline m_uiPipeline;
  wgpu::BindGroupLayout m_uiFrameLayout;
  wgpu::BindGroupLayout m_uiTextureLayout;
  wgpu::BindGroup m_uiFrameBindings;
  wgpu::Buffer m_uiUniforms;
  wgpu::Buffer m_uiInstances;
  std::size_t m_uiCapacity = 0;

  // Depth of field (postFocus.wgsl): the scene halved with each pixel's
  // blur, that blurred, and the scene with it mixed in (what the composite
  // reads then, by m_compositeFocusBindings).
  struct FocusTarget
  {
    wgpu::Texture texture;
    wgpu::TextureView view;
  };
  FocusTarget m_focusHalf;
  FocusTarget m_focusBlur;
  FocusTarget m_focusOut;
  wgpu::Buffer m_focusParams;
  wgpu::BindGroup m_focusPrepareBindings;
  wgpu::BindGroup m_focusBlurBindings;
  wgpu::BindGroup m_focusCombineBindings;
  wgpu::BindGroup m_compositeFocusBindings;
  wgpu::BindGroupLayout m_focusLayout;
  wgpu::RenderPipeline m_focusPreparePipeline;
  wgpu::RenderPipeline m_focusBlurPipeline;
  wgpu::RenderPipeline m_focusCombinePipeline;
  bool m_focused = false; // this frame went through depth of field

  // Ambient occlusion (meshDepthNormals.wgsl, ambientOcclusion.wgsl), at half
  // the viewport's size: what's drawn opaque (normals and depth, and a depth
  // buffer for it), the occlusion (and contact shadows) worked out from it,
  // and that blurred across (into `blurred`) and back down (into
  // `occlusion`, which the lit pass reads); each pass's parameters and
  // bindings (the occlusion's, across, down). Made the first time a frame has
  // it; until then the lit pass binds a 1x1 stand-in.
  struct OcclusionTargets
  {
    glm::ivec2 size{0, 0};
    wgpu::Texture normals;
    wgpu::TextureView normalsView;
    wgpu::Texture depth;
    wgpu::TextureView depthView;
    wgpu::Texture occlusion;
    wgpu::TextureView occlusionView;
    wgpu::Texture blurred;
    wgpu::TextureView blurredView;
    std::array<wgpu::Buffer, 3> params;
    std::array<wgpu::BindGroup, 3> bindings;
  };
  OcclusionTargets m_occlusion;
  wgpu::Texture m_noOcclusion;
  wgpu::TextureView m_noOcclusionView;
  wgpu::BindGroupLayout m_occlusionLayout;
  wgpu::RenderPipeline m_occlusionPipeline;
  wgpu::RenderPipeline m_occlusionBlurPipeline;
  bool m_occlusionBroken = false; // its passes didn't build (logged)
  bool m_occluded = false;        // this frame's lit pass reads it

  // Point lights and their clusters, on the GPU (capacities in elements).
  wgpu::Buffer m_lightBuffer;
  wgpu::Buffer m_clusterRangeBuffer;
  wgpu::Buffer m_clusterIndexBuffer;
  std::size_t m_lightCapacity = 0;
  std::size_t m_clusterRangeCapacity = 0;
  std::size_t m_clusterIndexCapacity = 0;

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
  // Terrain materials' surface (meshTerrainSurface.wgsl), built the first
  // time one is made; and their layers' texture arrays, by their textures.
  std::shared_ptr<const Surface> m_terrain;
  bool m_terrainBroken = false;
  struct CachedLayers
  {
    std::vector<std::weak_ptr<const TextureData>> sources;
    std::weak_ptr<GpuTexture> gpu;
  };
  std::unordered_map<std::string, CachedLayers> m_layerArrays;
  // Material graph surfaces by WGSL and alpha mode, while materials use them.
  std::unordered_map<std::string, std::weak_ptr<const Surface>> m_surfaces;
  // Ones that didn't build (already logged), so they aren't tried again.
  std::unordered_set<std::string> m_brokenSurfaces;
  // The surface whose pipeline the pass being encoded has set.
  WGPURenderPipeline m_boundPipeline = nullptr;

  // This frame's target, set by the backend.
  WGPUCommandEncoder m_encoder = nullptr;
  WGPUTextureView m_target = nullptr;
  glm::ivec2 m_viewport{0, 0};

  // This frame's work, gathered between beginFrame() and endFrame().
  FramePlan m_plan;
  PlannedMesh m_plannedMesh; // the last one finish() asked for
};

} // namespace sfs
