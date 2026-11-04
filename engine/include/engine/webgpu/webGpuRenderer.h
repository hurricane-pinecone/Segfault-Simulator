#pragma once

#include "engine/runtime/rendering/framePlan.h"
#include "engine/runtime/rendering/grassTiles.h"
#include "engine/runtime/rendering/iRenderer.h"
#include "engine/webgpu/wgpuHandle.h"

#include <webgpu/webgpu.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sfs
{

// A pass's timestamp writes: wgpu-native has a type for each kind of pass,
// the web's header one for both.
#ifdef __EMSCRIPTEN__
using RenderTimestamps = WGPUPassTimestampWrites;
using ComputeTimestamps = WGPUPassTimestampWrites;
#else
using RenderTimestamps = WGPURenderPassTimestampWrites;
using ComputeTimestamps = WGPUComputePassTimestampWrites;
#endif

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
  /**
   * Where this frame renders, with its target (the window's next picture)
   * asked for only when it's needed: the passes that don't draw into it
   * record into `encoder`; before the first that does, `acquire` runs, and
   * points the renderer (setTarget) at the encoder and target the rest record
   * into. Left without a target, those passes (composite, UI) are skipped.
   */
  void setLateTarget(WGPUCommandEncoder encoder,
                     glm::ivec2 size,
                     std::function<void()> acquire);

  MeshHandle createMesh(const MeshData& mesh) override;
  void destroyMesh(MeshHandle mesh) override;
  // Skinned copies: their own vertex buffer, deformed by a compute pass
  // (skinning.wgsl) before the frame's first pass in frames they're drawn
  // in after a new pose; drawn as any mesh after that, in every pass.
  MeshHandle createSkinnedMesh(MeshHandle mesh) override;
  void setSkinPose(MeshHandle skinned,
                   std::span<const glm::mat4> palette,
                   const Aabb& bounds) override;
  // Whether a handle is one of its meshes now (not destroyed, nor stale).
  bool hasMesh(MeshHandle mesh) const { return findMesh(mesh) != nullptr; }
  MaterialHandle createMaterial(const MeshMaterial& material) override;
  void updateMaterial(MaterialHandle handle,
                      const MeshMaterial& material) override;
  void destroyMaterial(MaterialHandle material) override;
  void setTerrainPuddles(MaterialHandle material,
                         const TerrainPuddles& puddles) override;
  void setTerrainDeformation(MaterialHandle material,
                             const TerrainDeformation& deformation) override;
  void writeTerrainDeformation(MaterialHandle material,
                               glm::ivec2 from,
                               std::span<const uint16_t> halves) override;
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
  void beginFeed(const FeedView& view, const UiTexture& into) override;
  bool textureShown(uint32_t id) const override;
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
    // The lit ones again, with back faces drawn too (double-sided materials).
    wgpu::RenderPipeline litTwoSided;
    wgpu::RenderPipeline blendTwoSided;
    wgpu::RenderPipeline additiveTwoSided;
    // Grass with this surface's look: built the first time grass uses it
    // (most surfaces never are), from its WGSL.
    std::string wgsl;
    // Whether its WGSL reads the time (a graph's may cut its see-through
    // parts out by it: their shadows change every frame).
    bool timed = false;
    // (One for each GrassDraw.)
    mutable std::array<wgpu::RenderPipeline, 6> grass;
    mutable std::array<bool, 6> grassBroken{};
    // Its depth and normals, for ambient occlusion: built the first time a
    // frame has it.
    mutable wgpu::RenderPipeline depthNormals;
    mutable wgpu::RenderPipeline depthNormalsTwoSided;
    mutable bool depthNormalsBroken = false;
    // Masked sections in the lit pass (see cutOutPipeline()): their depth
    // first, cut out, then lit where it's equal, writing none; built the
    // first time a frame has one.
    mutable wgpu::RenderPipeline cutOutDepth;
    mutable wgpu::RenderPipeline cutOutDepthTwoSided;
    mutable wgpu::RenderPipeline cutOutLit;
    mutable wgpu::RenderPipeline cutOutLitTwoSided;
    mutable bool cutOutBroken = false;
    // As its shadow, writing heights too, for rain's overhead map: built the
    // first time it rains.
    mutable wgpu::RenderPipeline rainMap;
    mutable bool rainMapBroken = false;
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
  // shadow's. And its blades kept in tiles (GrassTileCache): the tiles, their
  // blades, how many each has, and a frame's tiles (to build, to grow from),
  // with the tile passes' bindings.
  struct GpuGrass
  {
    GrassFieldState state; // its grids, trample bounds and blade capacity
    GpuImage ground;
    GpuImage shape;
    GpuImage data;
    GpuImage trample;
    // Its ground's terrain's splat map, for its ground past the maps (null:
    // none, a stand-in bound).
    std::shared_ptr<GpuTexture> farSplat;
    // Its ground's deformation window, which its blades stand on (null: not
    // deformed, a flat stand-in bound).
    std::shared_ptr<GpuTexture> deformMap;

    wgpu::Buffer blades;
    wgpu::Buffer draws;
    wgpu::Buffer growUniforms;
    wgpu::Buffer drawUniforms;
    wgpu::Buffer casters;
    wgpu::BindGroup growBindings;
    std::array<wgpu::BindGroup, 3> drawBindings;
    wgpu::BindGroup shadowBindings;
    std::unique_ptr<GrassTileCache> tiles;
    wgpu::Buffer kept;
    uint32_t keptChunks = 0; // room for them
    wgpu::Buffer tileState;  // by slot: its blades, and this frame's listed
    wgpu::Buffer tileList;
    uint32_t tileListRoom = 0; // entries
    wgpu::Buffer listedIn;     // this frame's blades' lists
    uint32_t listedRoom = 0;   // chunks
    wgpu::BindGroup tileBindings;
    // This frame: grown from its tiles (those to build, and those to grow
    // from), or all of it (cs_main's workgroups).
    bool fromTiles = false;
    uint32_t builds = 0;
    uint32_t tilesDrawn = 0;
    glm::uvec3 workgroups{0, 0, 0};

    bool grown = false;  // this frame: its blades
    bool placed = false; // this frame: its draw's uniforms (for its ground)
    bool groundPlaced = false; // likewise, kept for a camera feed's ground
    // A camera feed's blades (CameraFeedComponent::grassBlades), grown apart
    // from the view's: buffers and settings of its own (made when first
    // grown).
    struct Feed
    {
      std::array<uint32_t, 3> capacity{0, 0, 0};
      wgpu::Buffer blades;
      wgpu::Buffer draws;
      wgpu::Buffer casters; // (none cast: bound only)
      wgpu::Buffer growUniforms;
      wgpu::Buffer drawUniforms;
      wgpu::BindGroup growBindings;
      std::array<wgpu::BindGroup, 3> drawBindings;
      bool grown = false; // this frame
      glm::uvec3 workgroups{0, 0, 0};
    };
    std::unique_ptr<Feed> feed;
    bool casts = false;    // this frame: its blades into the sun's shadow
    uint32_t castInto = 0; // the cascades they're drawn into so far (bits)
  };

  // A material on the GPU: its surface, its parameters, and the bind group
  // (group 1 of its surface's pipelines) with its textures.
  struct GpuMaterial
  {
    AlphaMode alphaMode = AlphaMode::Opaque;
    bool doubleSided = false; // back faces drawn too
    std::shared_ptr<const Surface> surface;
    wgpu::Buffer uniforms;
    std::vector<std::shared_ptr<GpuTexture>> textures; // the bind group's
    wgpu::BindGroup bindings;
    uint64_t version = 0; // new each time it's made (nextVersion())
    // A terrain's, kept to make it again with new puddles.
    std::shared_ptr<const TerrainMaterial> terrain;
  };

  // A terrain material's deformation on the GPU: as set, its window of the
  // map (half floats, wrapping) and the ground under its patch (as given).
  struct GpuDeformation
  {
    TerrainDeformation settings;
    std::shared_ptr<GpuTexture> map;
    std::shared_ptr<GpuTexture> ground;
  };

  // A terrain material's puddles on the GPU: as set, the ground under their
  // window (as given), their map (found by findPuddles()) and its pass's
  // uniforms.
  struct GpuPuddles
  {
    TerrainPuddles settings;
    std::shared_ptr<GpuTexture> ground;
    std::shared_ptr<GpuTexture> map;
    std::shared_ptr<GpuTexture> fillRate; // its settings' (null: none)
    wgpu::Buffer uniforms;
    glm::ivec2 deformFrom{0}; // its deformation's window, when last found
    bool stale = true;        // to be found again this frame
  };

  struct GpuSection
  {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    GpuMaterial material; // the mesh's own
  };

  // A skinned copy's (createSkinnedMesh()): the original's vertices and
  // weights it's deformed from (references to its buffers), the bind group
  // its compute pass reads them and writes its vertices with, and its pose.
  struct GpuSkin
  {
    wgpu::Buffer source;
    wgpu::Buffer weights;
    wgpu::BindGroup bindings;
    uint32_t vertexCount = 0;
    uint32_t sourceSlot = 0; // the original's (destroyed with it)
    std::vector<glm::mat4> palette;
    bool dirty = false; // posed since its vertices were last deformed
  };

  struct GpuMesh
  {
    wgpu::Buffer vertices;
    wgpu::Buffer indices;
    uint32_t indexCount = 0;
    // Shared with its skinned copies (which draw with its materials).
    std::shared_ptr<std::vector<GpuSection>> sections;
    // A sphere around its vertices (for which point lights' reach it's in).
    glm::vec3 boundsCenter{0.0f};
    float boundsRadius = 0.0f;
    uint64_t version = 0; // new each time one's made (nextVersion())
    // A skinned mesh's: its vertices' weights (SkinWeights) and joints; and,
    // for a skinned copy, its skin.
    wgpu::Buffer skinWeights;
    uint32_t vertexCount = 0;
    uint32_t joints = 0;
    std::unique_ptr<GpuSkin> skin;
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
    return {mesh.sections->size(), mesh.boundsCenter, mesh.boundsRadius};
  }

  bool createPipelines();
  // Skinning's compute pass (skinning.wgsl); without it (logged), skinned
  // meshes draw as bound.
  void createSkinningPipeline();
  // Deform the skinned copies drawn this frame that were posed since.
  void encodeSkinning();
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
  // Group 1's layout for the standard surface's material bindings, its
  // parameters seen by the vertex stage too if `vertexToo` (foliage's).
  wgpu::BindGroupLayout standardMaterialLayout(bool vertexToo);
  // Whether the foliage surface is built (building it the first time).
  bool foliageSurface();
  // A material graph shader's surface for an alpha mode: shared while any
  // material uses it.
  std::shared_ptr<const Surface>
  graphSurface(const materialGraph::MaterialShader& shader, AlphaMode mode);
  std::optional<GpuMaterial> makeMaterial(const MeshMaterial& material);
  std::optional<GpuMaterial> makeStandardMaterial(const MeshMaterial& material);
  std::optional<GpuMaterial> makeGraphMaterial(const MeshMaterial& material);
  // `id`: the material slot's (0: a new one), for its puddle map
  // (setTerrainPuddles()) and deformation (setTerrainDeformation(); a
  // patch's are its ground's), if any.
  std::optional<GpuMaterial> makeTerrainMaterial(const MeshMaterial& material,
                                                 uint32_t id = 0);
  // A terrain material made again around its puddles and deformation (its
  // terrain as it is).
  void rebuildTerrain(uint32_t id);
  // A terrain material's uniforms written again (its puddles' or
  // deformation's windows moved), and its patch's.
  void writeTerrainUniforms(uint32_t ground);
  // A deformed terrain's (or its patch's) deformation, if it has one.
  const GpuDeformation* deformationOf(uint32_t id) const;
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
  // The lit pass's bindings, with these frame uniforms, light clusters and
  // occlusion map (the frame's, or a camera feed's).
  wgpu::BindGroup makeLitBindings(WGPUBuffer frameUniforms,
                                  WGPUBuffer ranges,
                                  uint64_t rangeBytes,
                                  WGPUBuffer indices,
                                  uint64_t indexBytes,
                                  WGPUTextureView occlusion);
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
  makeImage(WGPUDevice device,
            glm::ivec2 size,
            WGPUTextureFormat format,
            WGPUTextureUsage usage = WGPUTextureUsage_TextureBinding |
                                     WGPUTextureUsage_CopyDst);
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
  // What a grass pipeline draws: the ground under it, its nearest list of
  // blades (which find the air at their vertices, see meshGrass.wgsl), or
  // the others; the blades' depth alone (their pre-pass), and the two lists
  // again lit only where their depth is that (equal, writing none).
  enum class GrassDraw
  {
    Ground,
    NearBlades,
    Blades,
    Depth,
    NearBladesEqual,
    BladesEqual,
  };
  WGPURenderPipeline grassPipeline(const Surface& surface, GrassDraw draw);
  // Ambient occlusion (and contact shadows, worked out with it): whether this
  // frame has it (its passes and targets made, the lit bindings pointed at
  // its map), the passes working it out, and a surface's depth-and-normals
  // pipeline (null if it doesn't build).
  bool prepareOcclusion();
  void encodeOcclusion();
  WGPURenderPipeline depthNormalsPipeline(const Surface& surface,
                                          bool twoSided);
  // Grass tiles hidden behind what's drawn opaque left out: from ambient
  // occlusion's depth (when the frame has it), the farthest depth over the
  // screen's blocks (grassCulling.wgsl), its levels made for its size, and
  // worked out.
  void ensureGrassCulling();
  void encodeGrassCulling();
  void growGrass();
  // Find the puddles of terrains whose ground, deformation or settings
  // changed (puddles.h), before they're drawn.
  void findPuddles();
  void encodeGrass(WGPURenderPassEncoder pass);
  // The frame's grass into the sun's shadow map being drawn (group 0 bound at
  // `offset`, the map's).
  void encodeGrassShadows(WGPURenderPassEncoder pass,
                          uint32_t offset,
                          uint32_t cascade);
  void encodeComposite();

  // A camera feed (beginFeed()): its targets made at its size, its blades
  // grown (if it has them), its view drawn lit and composited into its
  // picture; and grass in its view (the ground, and its blades).
  void ensureFeedTargets(glm::ivec2 size);
  void growFeedGrass();
  void encodeFeed();
  void encodeFeedGrass(WGPURenderPassEncoder pass);

  // GPU-drawn textures (UiTexture::gpu): this frame's UI's made (if need be)
  // and marked shown, and those neither shown nor drawn a while let go.
  struct GpuRenderTexture;
  GpuRenderTexture* renderTexture(uint32_t id, glm::ivec2 size);
  void prepareRenderTextures();
  // Auto exposure's metering into m_exposure[0] (exposure.wgsl), and that
  // copied into m_exposure[1] for the next frame's.
  void encodeExposure();
  // UI (drawUi), over the target after the composite.
  bool createUiPipeline();
  void encodeUi();

  // Point-light shadows: the cube maps (room for `lights`, faces
  // `resolution` texels; rebuilding the lit bindings when remade), which
  // lights get them this frame and their casters, and drawing them.
  void ensurePointShadowMaps(uint32_t lights, uint32_t resolution);
  void encodePointShadows();
  // A point light's shadow casters' mesh and materials (a draw's) as they
  // are now, the same while they are (each caster's is this and its
  // instance); 0 if they change every frame (cut out by the time).
  uint64_t casterKey(const Draw& draw) const;
  // A mesh's or material's version: new each time.
  uint64_t nextVersion() { return ++m_versions; }
  void ensureInstanceCapacity(std::size_t instanceCount);
  void uploadFrame();
  void encodeShadowPass();
  // Rain: its overhead map (made full size the first time it's needed), drawn
  // from the sun's shadow casters (with a surface's pipeline for it, null if
  // it doesn't build); and its streaks and splashes, in the lit pass (their
  // pipeline built the first time).
  bool ensureRainMap();
  void encodeRainMap();
  WGPURenderPipeline rainMapPipeline(const Surface& surface);
  WGPURenderPipeline rainPipeline();
  void encodeRain(WGPURenderPassEncoder pass);
  void encodeLitPass();
  // Temporal anti-aliasing's resolve (taa.wgsl) of the lit scene with its
  // history, into m_taa[m_taaIndex], copied back over the scene.
  void encodeTaa();
  // Whether this frame is anti-aliased over time (and so jittered).
  bool temporalAntiAliasing() const;
  // Which of a draw's sections to draw: All (the shadow pass), Opaque (all but
  // Blend ones) or SeeThrough (Blend ones, or all for an allSections draw).
  enum class Sections
  {
    All, // the sun's shadow map
    Opaque,
    SeeThrough,
    LightShadow,  // a point light's shadow map (every section)
    DepthNormals, // ambient occlusion's first pass (as Opaque)
    CutOutDepth,  // the lit pass's masked sections' depth, before the rest
    RainMap,      // rain's overhead map (as All)
  };
  void encodeDraw(WGPURenderPassEncoder pass, const Draw& draw, Sections which);
  // A masked section's pipeline in the lit pass: its cut-out depth (drawn
  // before anything's lit), or lit where that depth is (testing for equal,
  // writing none: each pixel shaded once however many cut-out layers lie over
  // it). Null if they don't build (it's lit as the others are, then).
  WGPURenderPipeline
  cutOutPipeline(const Surface& surface, bool depthOnly, bool twoSided);
  // The pipeline a surface draws with in a pass (see Sections).
  static WGPURenderPipeline pipelineFor(const Surface& surface,
                                        Sections which,
                                        AlphaMode mode,
                                        bool twoSided);

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

  // Skinning: its pipeline, its shared bind group (every copy's palette, one
  // after another, and each dispatch's uniforms at a dynamic offset) and
  // the copies' (their source, weights and vertices); the buffers, grown as
  // needed; and the copies drawn this frame (by slot).
  wgpu::BindGroupLayout m_skinningSharedLayout;
  wgpu::BindGroupLayout m_skinningLayout;
  wgpu::ComputePipeline m_skinningPipeline;
  wgpu::Buffer m_skinPalettes;
  uint64_t m_skinPaletteCapacity = 0; // matrices
  wgpu::Buffer m_skinUniforms;
  uint64_t m_skinUniformCapacity = 0; // dispatches
  wgpu::BindGroup m_skinningShared;
  std::vector<uint32_t> m_skinsDrawn;

  // Mesh slots; a handle's id is its slot index + 1 (0 stays the null handle).
  std::vector<std::optional<GpuMesh>> m_meshes;
  std::vector<uint32_t> m_meshGenerations; // per slot (see meshSlotOf())
  std::vector<uint32_t> m_freeSlots;

  // Objects' own materials (createMaterial), in slots like meshes.
  std::vector<std::optional<GpuMaterial>> m_materials;
  // Terrain materials' puddles, by material id (setTerrainPuddles()), and
  // what finds them: its passes (puddles.wgsl) and their two work textures.
  std::unordered_map<uint32_t, GpuPuddles> m_puddles;
  wgpu::BindGroupLayout m_puddleLayout;
  std::array<wgpu::ComputePipeline, 6> m_puddlePipelines;
  std::array<GpuImage, 2> m_puddleWork;
  // Deformed terrain materials' deformation (setTerrainDeformation()), by
  // material id, and their patches' materials' ground's id.
  std::unordered_map<uint32_t, GpuDeformation> m_deformations;
  std::unordered_map<uint32_t, uint32_t> m_patches;
  std::vector<uint32_t> m_freeMaterialSlots;
  std::vector<std::optional<GpuGrass>> m_grass;
  std::vector<uint32_t> m_freeGrassSlots;
  wgpu::BindGroupLayout m_grassLayout;
  wgpu::BindGroupLayout m_grassGrowLayout;
  wgpu::ComputePipeline m_grassGrowPipeline;
  // Growing from tiles kept built (meshGrassGrow.wgsl's cs_build, then
  // cs_count, cs_order and cs_grow, listing them front to back); or not
  // (SFS_GRASS_UNCACHED: all of them every frame, as cs_main). With
  // SFS_GRASS_TILES, tiles built lately are tinted.
  wgpu::BindGroupLayout m_grassTileLayout;
  wgpu::ComputePipeline m_grassBuildPipeline;
  wgpu::ComputePipeline m_grassCountPipeline;
  wgpu::ComputePipeline m_grassOrderPipeline;
  wgpu::ComputePipeline m_grassTilePipeline;
  bool m_grassUncached = false;
  bool m_grassTilesShown = false;
  uint64_t m_grassFrame = 0;
  wgpu::BindGroupLayout m_grassShadowLayout;
  wgpu::RenderPipeline m_grassShadowPipeline;

  // Counting grass's work, for measuring it (SFS_GRASS_STATS, natively): the
  // grow pass counts its blades at each of its steps, all fields together,
  // into m_grassCounters (see meshGrassGrow.wgsl), and the device how many
  // vertices and fragments its blades' draws shade (where it can count
  // them); with SFS_GRASS_STATS=2, the blades' pass also counts the
  // fragments it draws at each pixel (which keeps the GPU from skipping
  // hidden ones early, so it's slower). Now and then they're read back, and
  // their averages logged.
  static constexpr uint32_t kGrassCounters = 20;
  static constexpr uint32_t kGrassQueries = 8;    // fields counted a frame
  static constexpr uint32_t kGrassStatistics = 3; // each one's numbers
  struct GrassStats
  {
    bool pixels = false;    // counting each pixel's fragments
    wgpu::QuerySet queries; // the device's counts, each field's blades
    wgpu::Buffer resolved;
    uint32_t queried = 0; // this frame's fields
    uint32_t copiedQueries = 0;
    std::array<double, kGrassStatistics> statistics{};
    enum class Readback
    {
      Idle,
      Copied,
      Mapping,
      Mapped,
    };
    wgpu::Buffer fragments; // a count a pixel, the lit pass's size
    glm::ivec2 size{0};
    wgpu::Buffer readback; // the counters, then the fragments
    glm::ivec2 copiedSize{0};
    Readback state = Readback::Idle;
    uint32_t readbacks = 0; // since the last log
    std::array<double, kGrassCounters> counters{};
    double fragmentsShaded = 0.0;
    double pixelsCovered = 0.0;
    double screenPixels = 0.0;
    std::array<double, 6> overdraw{}; // pixels shaded 1, 2-3, 4-7, 8-15,
                                      // 16-31, 32+ times
    // Kept tiles, all fields' a frame, summed since the last log: grown
    // from, built, kept, no room for, and their blades' room in use and in
    // all (bytes).
    std::array<double, 6> tiles{};
    uint32_t tileFrames = 0;
  };
  std::unique_ptr<GrassStats> m_grassStats;
  wgpu::Buffer m_grassCounters; // bound to the grow pass, counting or not
  void beginGrassStats();
  void copyGrassStats();
  void readGrassStats();

  // Timing the frame's passes on the GPU, for measuring what they cost: with
  // SFS_GPU_TIMING set, on a device with timestamp queries, their averages
  // are logged every few seconds. Each pass writes a timestamp as it begins
  // and ends, under the label of what's encoding it (m_passLabel; passes of
  // one label add up); every few frames they're copied out and read back.
  static constexpr uint32_t kTimedPasses = 128; // the most timed in a frame
  struct GpuTiming
  {
    enum class Readback
    {
      Idle,
      Copied,  // this frame's copied out, to map once it's submitted
      Mapping, // waiting on the map
      Mapped,
    };
    struct Row
    {
      std::string label;
      double total = 0.0;  // milliseconds, over the frames read back
      uint32_t frames = 0; // the frames it ran in
      bool nested = false; // a span inside a pass (counted in the pass's)
    };
    wgpu::QuerySet queries;
    wgpu::Buffer resolved;
    wgpu::Buffer readback;
    Readback state = Readback::Idle;
    std::array<const char*, kTimedPasses> labels{}; // this frame's passes
    std::array<bool, kTimedPasses> nested{};
    uint32_t used = 0;
    std::array<const char*, kTimedPasses> copiedLabels{}; // the frame read back
    std::array<bool, kTimedPasses> copiedNested{};
    uint32_t copied = 0;
    int openSpan = -1;     // the span inside a pass begun, not yet ended
    std::vector<Row> rows; // in the order first seen
    double span = 0.0;     // first pass's start to last's end, summed
    uint32_t readbacks = 0;
    RenderTimestamps renderWrites{};
    ComputeTimestamps computeWrites{};
  };
  std::unique_ptr<GpuTiming> m_timing;
  const char* m_passLabel = "other";
  // A pass's timestamp writes (null when not timing), under m_passLabel; the
  // frame's read back (at its start), and copied out (at its end).
  const RenderTimestamps* timedRenderPass();
  const ComputeTimestamps* timedComputePass();
  bool nextTimedPass();
  // A span of draws inside a pass, timed under `label` (where the device can
  // time inside passes); ended by the next span, or endTimedSpan().
  void beginTimedSpan(WGPURenderPassEncoder pass, const char* label);
  void endTimedSpan(WGPURenderPassEncoder pass);
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
  // dynamic offset's alignment), for the shadow pass; then rain's overhead
  // map's.
  wgpu::Buffer m_cascadeUniforms;

  // Rain's overhead map (RainMap: depth looking straight down), and its
  // heights (a float's bits: OpenGL can only compare against a depth
  // texture, not read it), while the weather has rain (until then 1x1
  // stand-ins, never read); whether they're the real ones, and drawn this
  // frame. Rain's streaks and splashes' pipeline (rain.wgsl), and whether it
  // failed to build (logged).
  wgpu::Texture m_rainMap;
  wgpu::TextureView m_rainMapView;
  wgpu::Texture m_rainHeights;
  wgpu::TextureView m_rainHeightsView;
  bool m_rainMapFull = false;
  bool m_rained = false;
  wgpu::RenderPipeline m_rainPipeline;
  bool m_rainBroken = false;

  // A physical sky (sky.wgsl): its transmittance table (from the CPU's
  // PhysicalSky), the sky rendered octahedrally (the background and haze
  // read it), the same with its clouds (what its reflections are blurred
  // from), its reflections (mip 0 sharp to the last, roughest), and the
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
  SkyTarget m_skyCloudy;
  SkyTarget m_skyReflections;
  wgpu::Texture m_brdfTable;
  wgpu::TextureView m_brdfTableView;
  wgpu::Sampler m_skySampler;
  wgpu::BindGroupLayout m_skyLayout;
  wgpu::Buffer m_skyUniforms;
  // The radiance's, each mip's, the clouds'.
  std::vector<wgpu::Buffer> m_skyPassUniforms;
  std::vector<wgpu::BindGroup> m_skyPassBindings;
  wgpu::RenderPipeline m_skyRadiancePipeline;
  wgpu::RenderPipeline m_skyCloudsPipeline;
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
    bool used = false; // this frame
  };
  std::unordered_map<uint32_t, GpuUiTexture> m_uiTextures;
  GpuUiTexture m_uiWhite; // what a batch with no texture in a place binds
  // A batch's glyph and image textures bound, by their ids (glyphs' << 32;
  // 0: white); forgotten when a texture's made again or let go.
  std::unordered_map<uint64_t, wgpu::BindGroup> m_uiBindings;
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
  // Auto exposure: its correction (one pixel, what the composite reads), and
  // last frame's (what metering eases from).
  std::array<wgpu::Texture, 2> m_exposure;
  std::array<wgpu::TextureView, 2> m_exposureView;
  wgpu::BindGroupLayout m_exposureLayout;
  wgpu::RenderPipeline m_exposurePipeline;
  wgpu::Buffer m_exposureParams;
  wgpu::BindGroup m_exposureBindings;
  std::optional<std::chrono::steady_clock::time_point> m_exposureTime;
  // Temporal anti-aliasing: this frame's blend and the last (ping-ponged),
  // the camera as last frame saw (unset: no history), and this frame's
  // jittered one.
  std::array<wgpu::Texture, 2> m_taa;
  std::array<wgpu::TextureView, 2> m_taaView;
  std::array<wgpu::BindGroup, 2> m_taaBindings;
  wgpu::BindGroupLayout m_taaLayout;
  wgpu::RenderPipeline m_taaPipeline;
  wgpu::Buffer m_taaParams;
  std::size_t m_taaIndex = 0;
  uint32_t m_taaFrame = 0;
  std::optional<glm::mat4> m_taaPrevious;
  glm::mat4 m_jitteredViewProjection{1.0f};
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
  // The farthest depths over the screen's blocks, for grass (see
  // ensureGrassCulling()): its levels (one 1 x 1 till there's ambient
  // occlusion's depth), the passes working each out (the first reading that
  // depth, each next the level before) and their settings; whether this
  // frame has them; or never (SFS_GRASS_UNCULLED).
  struct GrassCulling
  {
    glm::ivec2 size{0, 0}; // its finest level's
    glm::ivec2 from{0, 0}; // the depths' it was made for
    wgpu::Texture texture;
    wgpu::TextureView all;
    std::vector<wgpu::TextureView> levels;
    std::vector<wgpu::BindGroup> bindings;
    wgpu::Buffer params;
    wgpu::Texture none; // a 1 x 1 stand-in
    wgpu::TextureView noneView;
  };
  GrassCulling m_grassCulling;
  wgpu::BindGroupLayout m_grassCullingLayout;
  wgpu::RenderPipeline m_grassCullingPipeline;
  bool m_grassCulled = false;
  bool m_grassUnculled = false;
  // Grass's ground drawn without its blades (SFS_GRASS_GROUND_ONLY), to
  // compare the two.
  bool m_grassGroundOnly = false;
  // Grass blades' depth pre-pass (SFS_GRASS_PREPASS): 1 on, 0 off, -1 on and
  // off two frames each (for comparing screenshots); over how many of the
  // nearest lists.
  int m_grassPrepass = 1;
  uint32_t m_grassPrepassLists = 1;
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
  // Each cube's still casters (those unchanged a while), drawn into a cube
  // of their own (m_pointShadowStillFaces) and kept while they're as they
  // were: each frame it's copied into the cube the lit pass reads (when
  // anything's changed) and only casters on the move drawn over it.
  // And what's changed that the plan's casters don't show: the ground moved
  // over world XY rectangles (min, max), and anything else at all
  // (m_shadowsEpoch, e.g. a terrain's window moving; m_shadowTextures,
  // textures a see-through caster may cut out by).
  struct StillShadows
  {
    uint64_t light = 0; // what it was drawn for (where, its reach...); 0: none
    std::unordered_set<uint64_t> kept; // the casters in it (by key)
    // Each of the light's casters (by key): how many frames it's been as it
    // is.
    std::unordered_map<uint64_t, uint32_t> still;
    bool copied = false; // the lit pass's cube is it, with nothing over it
  };
  wgpu::Texture m_pointShadowStill;
  std::vector<wgpu::TextureView> m_pointShadowStillFaces;
  std::vector<StillShadows> m_stillShadows;
  std::vector<glm::vec4> m_shadowGroundMoved;
  uint64_t m_shadowsEpoch = 0;
  uint64_t m_shadowTextures = 0;
  uint64_t m_versions = 0;

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
  // The foliage surface (MeshMaterial::foliage), built the first time a
  // material needs it (null meanwhile, or if it doesn't build).
  std::shared_ptr<const Surface> m_foliage;
  bool m_foliageBroken = false;
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
  // Asks for the target, before the first pass into it (setLateTarget).
  std::function<void()> m_acquire;
  bool acquireTarget();
  glm::ivec2 m_viewport{0, 0};

  // This frame's work, gathered between beginFrame() and endFrame().
  FramePlan m_plan;
  PlannedMesh m_plannedMesh; // the last one finish() asked for
  // Whose draws encodeDraw() is drawing (their materials): the frame's, or
  // its camera feed's.
  const FramePlan* m_drawing = &m_plan;

  // GPU-drawn textures (RenderTexture), by UiTexture id: the picture (in
  // the window's format, so the composite draws into it), what the UI reads
  // it through (without sRGB decoding, as its other images), and the frames
  // it was last shown in a UI and last shown or drawn.
  struct GpuRenderTexture
  {
    wgpu::Texture texture;
    wgpu::TextureView view;
    GpuUiTexture ui;
    uint64_t shown = 0;
    uint64_t used = 0;
  };
  std::unordered_map<uint32_t, GpuRenderTexture> m_renderTextures;
  uint64_t m_frameCount = 0; // endFrame()s with a target

  // This frame's camera feed (beginFeed()): its view, its picture (id, size)
  // and its draws; its targets (multisampled as the lit pass's pipelines
  // are; the glow's only drawn into), uniforms, instances, light clusters
  // and bindings (the lit pass's, but for these).
  std::optional<FeedView> m_feed;
  uint32_t m_feedInto = 0;
  FramePlan m_feedPlan;
  struct FeedTargets
  {
    glm::ivec2 size{0, 0};
    wgpu::Texture depth;
    wgpu::TextureView depthView;
    wgpu::Texture samples; // the scene's, multisampled (none with one sample)
    wgpu::TextureView samplesView;
    wgpu::Texture glow;
    wgpu::TextureView glowView;
    wgpu::Texture scene; // resolved, what the composite reads
    wgpu::TextureView sceneView;
  };
  FeedTargets m_feedTargets;
  wgpu::Buffer m_feedUniforms;
  wgpu::Buffer m_feedCompositeParams;
  wgpu::Buffer m_feedInstances;
  std::size_t m_feedInstanceCapacity = 0;
  wgpu::Buffer m_feedRanges;
  wgpu::Buffer m_feedIndices;
  std::size_t m_feedRangeCapacity = 0;
  std::size_t m_feedIndexCapacity = 0;
  wgpu::BindGroup m_feedLitBindings;
};

} // namespace sfs
