#pragma once

#include "engine/core/rendering/deformation.h"
#include "engine/core/rendering/grass.h"
#include "engine/core/rendering/lightClusters.h"
#include "engine/core/rendering/mesh.h"
#include "engine/core/rendering/puddles.h"
#include "engine/core/rendering/terrain.h"
#include "engine/runtime/rendering/frame.h"
#include "engine/runtime/rendering/uiDraw.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include "glm/glm/ext/vector_int4.hpp"
#include "glm/glm/ext/vector_uint2.hpp"
#include "glm/glm/ext/vector_uint4.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace sfs
{

/**
 * Most point lights with shadows at once (ShadowSettings::pointLights is
 * capped to it): six cube faces each fit a texture array's 256 layers.
 */
inline constexpr uint32_t kMaxShadowedPointLights = 32;

// The engine's shaders' uniforms and buffer elements, laid out as the WGSL
// declares them (16-byte aligned), for every backend to upload as they are.

/** `Frame` in meshCommon.wgsl. */
struct FrameUniforms
{
  glm::mat4 viewProjection;
  glm::mat4 lightViewProjection;
  glm::vec4 cameraPosition;
  glm::vec4 sunDirection;
  glm::vec4 sunColor;
  glm::vec4 shadow;
  glm::vec4 sky;
  glm::vec4 time;           // x seconds
  glm::vec4 cameraForward;  // xyz
  glm::uvec4 clusterSize;   // tiles across, down, slices; lights
  glm::vec4 clusterDepth;   // near, far, slices per log(depth / near), tile px
  glm::vec4 wind;           // which way it blows (unit xy), strength, gustiness
  glm::vec4 haze;           // strength, 1 / its half distance
  glm::vec4 sunBalance;     // the sun's share x rgb; w 1 if re-balanced
  glm::vec4 ambientBalance; // ambient's share x rgb; w the brightest light
                            // gets (1 unless tone mapped)
  glm::mat4 inverseViewProjection; // the background's view rays
  // The sun's shadow cascades (cascadeInfo.x > 1; else the one map above).
  std::array<glm::mat4, kMaxShadowCascades> cascades;
  std::array<glm::vec4, kMaxShadowCascades> cascadeRects;  // in the atlas (uv):
                                                           // corner, size
  std::array<glm::vec4, kMaxShadowCascades> cascadeParams; // world texel, depth
                                                           // bias, spread (uv)
  glm::vec4 cascadeInfo; // how many, the blend band (of a half-size), where
                         // the last starts fading out (of its reach)
  glm::vec4 skyMode;     // physical (0/1), reflections ready (0/1), their
                         // roughest mip, the night glow's strength
  glm::vec4 sunLight;    // rgb the physical sun's light; w its disk's radius
  glm::vec4 skyScale; // rgb a style's scale on the sky; w the disk's brightness
  std::array<glm::vec4, 9> skyIrradiance; // SH9 (SkyIrradiance)
  glm::vec4 skyLight; // x what eyes adapted to dusk and night scale the sky's
                      // light by (its reflections'; the rest is scaled), y
                      // how much of the sun's light clears the horizon, z
                      // what they scale lamps by (point lights, emission:
                      // PhysicalSky::lampScale)
  // Ambient occlusion: x on (0/1; its map is read, for it or contact
  // shadows), y how near in depth (of a pixel's) the map's must be to count,
  // z contact shadows on (0/1; the map's b holds them).
  glm::vec4 ambientOcclusion;
  glm::vec4 fog;      // height fog: density, height falloff, base height,
                      // start distance
  glm::vec4 fogLight; // x the most it hides, y its glow toward the sun, z on
                      // (0/1), w how far away the background is
  // A physical sky's moon: xyz toward it, w its disk's radius (0: none
  // drawn); xyz toward the sun (its lit side), w how much of it is lit;
  // rgb its disk's light (not adapted), w 1 if it's the frame's light (in
  // the sun's place: sunDirection, sunLight).
  glm::vec4 moon;
  glm::vec4 moonSun;
  glm::vec4 moonLight;
  // The weather (SceneWeather): rain, wetness, puddles, cloud cover (each
  // 0..1); the wind (xy, m/s) and how far the clouds have drifted (zw, km).
  glm::vec4 weather;
  glm::vec4 weatherWind;
  // A physical sky's clouds (none drawn while cloudBase.w is 0): rgb the
  // light on their tops (the sun's at their height, or the moon's), w how
  // dark they are; rgb their undersides lit through (both not adapted), w
  // 1 / the share of the sun's light they let through on average (sunLight
  // is dimmed by it; their shadows put it back where they aren't).
  glm::vec4 cloudLight;
  glm::vec4 cloudBase;
  // Rain (RainSettings, while the weather has it): the overhead map's
  // view-projection (rainMapFor()); its centre (xy), half size, w 1 if it's
  // drawn this frame; its top, depth range, and the streaks and splashes
  // drawn (counts); a streak's length (m), width (m) and brightness, how
  // strongly drops ring water; the veil's density, the viewport (pixels).
  glm::mat4 rainView;
  glm::vec4 rainMap;
  glm::vec4 rainDepth;
  glm::vec4 rainStreaks;
  glm::vec4 rainVeil;
  // How far rain's drops have drifted (xy, m; FrameParams::rainDrift) and
  // the wind they slant in (zw, m/s; eased).
  glm::vec4 rainDrift;
  // Water dripping off edges (rainDrips()): how many places it's looked
  // for (x, a count), the share that drip each round (y), how much they trickle
  // (z), how wet the drip line is (w).
  glm::vec4 rainDrips;
  // Lightning (FrameLightning; none while lightningGlow.w is 0): its
  // channel's bottom (xyz), w the point light casting its shadow (-1:
  // none); its top, where the cloud base glows (xyz), w 1 if the channel
  // lights; rgb a metre of the channel's light, w how wide the glow is (m);
  // rgb the cloud base's glow at its middle, w 1 while it flashes. Its light
  // adapted, as the sky's.
  glm::vec4 lightningLow;
  glm::vec4 lightningHigh;
  glm::vec4 lightningLight;
  glm::vec4 lightningGlow;
};
static_assert(sizeof(FrameUniforms) == 64 * 2 + 16 * 13 +
                                           64 * (2 + kMaxShadowCascades) +
                                           16 * (2 * kMaxShadowCascades + 4 +
                                                 9 + 1 + 3 + 3 + 4 + 4 + 6),
              "FrameUniforms must match the WGSL Frame layout");

/**
 * `Sky` in sky.wgsl: a physical sky's atmosphere (PhysicalSky), for the
 * passes rendering its reflections.
 */
struct SkyUniforms
{
  glm::vec4 toSun;    // xyz; w the solar irradiance
  glm::vec4 rayleigh; // rgb scattering per km; w its scale height
  glm::vec4 mie;      // scattering, extinction, scale height, g
  glm::vec4 ozone;    // rgb absorption per km
  glm::vec4 ground;   // rgb what the ground reflects
  glm::vec4 night;    // rgb the night glow at the zenith (the moon's in it)
  glm::vec4 planet;   // ground radius, top radius, viewer altitude, multiple
                      // scattering's share
  // Its clouds (as FrameUniforms' cloudLight and cloudBase): the camera's
  // place in their pattern (xy, km), cover, darkness; the light on their
  // tops (rgb; w the layer's altitude, km); their undersides; toward that
  // light (xyz).
  glm::vec4 clouds;
  glm::vec4 cloudLight;
  glm::vec4 cloudBase;
  glm::vec4 cloudToLight;
};

/** `Exposure` in exposure.wgsl: auto exposure's metering and easing. */
struct ExposureUniforms
{
  glm::vec4 params; // the target (log2 of the average brightness it aims
                    // for), the most it darkens and brightens (stops), how
                    // far toward what it wants it eases this frame (0..1)
  glm::vec4 source; // the scene's size (texels), on (0/1)
};

/** `Taa` in taa.wgsl: temporal anti-aliasing's resolve. */
struct TaaUniforms
{
  glm::mat4 inverseViewProjection;  // this frame's, not jittered
  glm::mat4 previousViewProjection; // last frame's, not jittered
  glm::vec4 params; // 1 / the size (texels), history valid (0/1), how much
                    // of this frame goes in
};

/** `SkyPass` in sky.wgsl: one reflection mip's. */
struct SkyPassUniforms
{
  glm::vec4 params; // roughness, the source's size (texels), samples
};

/** The size of a sun shadow atlas holding `cascades` maps of `tile` texels. */
inline glm::uvec2 shadowAtlasSize(uint32_t cascades, uint32_t tile)
{
  return {cascades > 1 ? tile * 2 : tile, cascades > 2 ? tile * 2 : tile};
}

/** `PointLight` in meshLit.wgsl (a storage buffer's elements). */
struct GpuPointLight
{
  glm::vec4 positionRadius;
  glm::vec4 colorIntensity;
  glm::vec4 shadow; // its cube's layer (-1: none), 1 / its face size, its size
};

/** `ShadowFace` in meshPointShadow.wgsl. */
struct FaceUniforms
{
  glm::mat4 viewProjection;
  glm::vec4 light; // position, 1 / reach
};

/** `Grass` in meshGrass.wgsl. */
struct GrassUniforms
{
  glm::vec4 trample; // the trample map's world min, 1 / its world size
  glm::vec4 blade;   // stiffness, draw distance, how much the ground's tinted,
                     // full height
  glm::vec4 maps;    // the maps' grid min, 1 / its size (0: unpainted)
  glm::vec4 shadow;  // self-shadow strength, how far blades cast the sun's
                     // shadow (0: none), how much wider than a blade, 0
  glm::mat4 model;
  glm::mat4 normalMatrix;
  glm::mat4 inverseModel;
  glm::vec4 vertexSun; // where blades start finding the sun's shadow at their
                       // vertices, over how far (see meshGrass.wgsl), 0, 0
  // Its ground past the maps (grassFarGround()): how much its terrain's
  // first layer is meadow, 1 with a terrain, where the blades start thinning
  // out to none (world units), 0; and how much each of the terrain's splat
  // map's channels' layers is.
  glm::vec4 far{0.0f};
  glm::vec4 farMeadow{0.0f};
};

/** `Grow` in meshGrassGrow.wgsl. */
struct GrassGrowUniforms
{
  glm::mat4 model;
  glm::mat4 normalMatrix;
  std::array<glm::vec4, 6> planes;
  glm::vec4 camera; // xyz, draw distance
  glm::vec4 ground; // the ground grid's min, texel size
  glm::ivec4 groundSize;
  glm::vec4 maps;      // the maps' grid min, 1 / its size
  glm::vec4 shape;     // height, variation, width, finest cell
  glm::vec4 reach;     // each lattice's
  glm::vec4 detail;    // 4 segments to, 2 to, casting shadows to (0: none),
                       // counting (1)
  glm::vec4 thinning;  // all kept to, how fast fewer past it, fewer still
                       // from (none at the draw distance), 1 (0: the old
                       // way, by lattice)
  glm::uvec4 capacity; // each list's, and the shadow casters'
  std::array<glm::ivec4, 4> lattices; // first cell, count
  // Grown from tiles kept built (GrassTileCache): the tiles to build, where
  // those drawn from start, how many of those, and their chunks.
  glm::uvec4 tiles;
  // Tiles hidden (behind what's drawn opaque) left out: the view, where it's
  // from and which way it looks (the camera's own, not grass's), and the
  // farthest depths over the screen's blocks: their finest level's size
  // (texels) and how many levels (0: none, nothing's left out).
  glm::mat4 viewProjection{1.0f};
  glm::vec4 eye{0.0f};
  glm::vec4 forward{0.0f};
  glm::vec4 hidden{0.0f};
};
static_assert(sizeof(GrassGrowUniforms) == 560,
              "GrassGrowUniforms must match meshGrassGrow.wgsl's Grow");

/** A grown blade (meshGrassGrow.wgsl's `GrownBlade`), in bytes. */
inline constexpr uint32_t kGrownBladeBytes = 32;

/** `QuadIn` in ui.wgsl: a UiQuad as the GPU reads it. */
struct UiInstance
{
  glm::vec4 rect;
  glm::vec4 uv;
  glm::vec4 color;
  glm::vec4 borderColor;
  glm::vec4 shape; // radius, softness, border, kind
};

/**
 * `Occlusion` in ambientOcclusion.wgsl: ambient occlusion's passes' (which
 * work out contact shadows too).
 */
struct OcclusionUniforms
{
  glm::mat4 inverseViewProjection;
  glm::mat4 viewProjection; // contact shadows' rays onto the screen
  glm::vec4 camera;         // xyz position, w 1 if a perspective view
  glm::vec4 forward;        // xyz view direction
  glm::vec4 params;  // radius (world), intensity, samples, pixels a world unit
                     // spans at depth 1 (orthographic: at any)
  glm::vec4 size;    // the pass's size (texels), 1 / that
  glm::vec4 blur;    // a blur's step (texels); where it fades with distance
                     // from, and where it's gone
  glm::vec4 light;   // xyz toward the frame's light (the sun's or the moon's);
                     // w ambient occlusion on (0/1)
  glm::vec4 contact; // contact shadows: length, thickness (world), strength
                     // (0: none), where they've faded out with distance
};
static_assert(sizeof(OcclusionUniforms) == 64 * 2 + 16 * 7,
              "OcclusionUniforms must match ambientOcclusion.wgsl");

/** `Focus` in postFocus.wgsl. */
struct FocusUniforms
{
  glm::vec4 depth; // projection[2][2], projection[3][2], perspective (1/0)
  glm::vec4 focus; // distance, half the sharp range, blur radius (texels)
  glm::vec4 texel; // a half-size texel (uv)
};

/** `Post` in post.wgsl. */
struct PostUniforms
{
  glm::vec4 texel; // xy the source's texel size
  glm::vec4 bloom; // threshold, softness, radius, intensity
  // The composite's grading (see ColorGrading):
  glm::vec4 tone;  // tone mapper (ToneMapper), exposure (x), contrast,
                   // saturation
  glm::vec4 grade; // graded (0: only clamped, as ever), vignette, its
                   // softness, the aspect (width / height)
  std::array<glm::vec4, 3> balance; // white balance (columns of a 3x3)
  glm::vec4 lift;
  glm::vec4 gamma; // 1 / gamma
  glm::vec4 gain;
};
static_assert(sizeof(PostUniforms) == 16 * 10,
              "PostUniforms must match the WGSL Post layout");

/** `Material` in meshCommon.wgsl: the standard surface's parameters. */
struct MaterialUniforms
{
  glm::vec4 baseColor;
  glm::vec4 params;      // normal scale, metallic, roughness, alpha cutoff
  glm::vec4 uvTransform; // tiling xy, offset zw
  glm::uvec4 mode;       // x: AlphaMode, y: unlit
};

/** A standard-surface material's uniforms. */
MaterialUniforms standardMaterialUniforms(const MeshMaterial& material);

/** `Terrain` in meshTerrainSurface.wgsl: a terrain material's parameters. */
struct TerrainUniforms
{
  std::array<glm::vec4, kMaxTerrainLayers> layers; // rgb tint, 1 / tile size
  std::array<glm::vec4, kMaxTerrainLayers>
      detail;       // roughness, normal scale, meadow, triplanar exponent
  glm::vec4 params; // layers, blend sharpness, anti-tiling (0/1), variation
  // Its puddle map (found on the GPU, see puddles.h): xy the world XY of its
  // corner, zw 1 / the size it covers (0: none); then whether it has puddles
  // (0/1) and the deepest water the map holds (kPuddleDepthRange); and each
  // layer's puddles (the first four, then the fifth).
  glm::vec4 puddleMap{0.0f};
  glm::vec4 puddleParams{0.0f};
  std::array<glm::vec4, 2> puddleLayers{};
  // Its deformation (setTerrainDeformation()): x 0 none, 1 the ground, 2
  // its patch; y the map's texel, z 1 / the window's size (m), w the map's
  // deepest. Then the window's valid corner (xy, world) and size (zw); the
  // patch's centre (xy), half size and band; the ground under it: its
  // corner (xy) and texel (z).
  glm::vec4 deformMap{0.0f};
  glm::vec4 deformWindow{0.0f};
  glm::vec4 deformPatch{0.0f};
  glm::vec4 deformGround{0.0f};
};
static_assert(sizeof(TerrainUniforms) == 16 * (2 * kMaxTerrainLayers + 9),
              "TerrainUniforms must match meshTerrainSurface.wgsl");

/**
 * A terrain material's uniforms, with its puddle map (setTerrainPuddles())
 * and its deformation (setTerrainDeformation(); as its patch draws it with
 * `patch`) where it has them.
 */
TerrainUniforms
terrainMaterialUniforms(const TerrainMaterial& terrain,
                        const TerrainPuddles* puddles = nullptr,
                        const TerrainDeformation* deformation = nullptr,
                        bool patch = false);

/**
 * A material graph's parameters, in its shader's order (at least one; missing
 * ones 0): its uniform buffer.
 */
std::vector<glm::vec4> graphMaterialParameters(const MeshMaterial& material);

/** What a material whose graph doesn't build draws with: magenta. */
MeshMaterial brokenMaterial();

/**
 * A view from a point light into cube face `face` (+X, -X, +Y, -Y, +Z, -Z, as
 * a cube texture's layers go), laid out as the GPU samples cube maps.
 */
glm::mat4 cubeFaceView(int face, const glm::vec3& light);

/** What a frame plan needs to know of a mesh a backend holds. */
struct PlannedMesh
{
  std::size_t sections = 0;
  // A sphere around its vertices (for which point lights' reach it's in).
  glm::vec3 boundsCenter{0.0f};
  float boundsRadius = 0.0f;
};

/**
 * A frame worked out on the CPU, the same for every backend: which
 * instances draw in which pass (lit, see-through back to front, the sun's
 * shadow, each shadowed point light's), the point lights and the clusters
 * they light, and every pass's uniforms. A backend feeds it the frame's draws
 * (IRenderer's), then uploads what it worked out and records its passes.
 */
class FramePlan
{
public:
  /**
   * One instanced draw: a mesh (by the backend's slot), its run of instances,
   * and its sections' materials in place of their own (a run of
   * drawMaterials(); 0: the section's own).
   */
  struct Draw
  {
    uint32_t meshSlot = 0;
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
    uint32_t firstMaterial = 0;
    uint32_t materialCount = 0;
    bool allSections = false; // see-through: every section, not just Blend's
  };

  /** A shadowed point light (its cube is its index) and its casters. */
  struct ShadowedLight
  {
    std::size_t light = 0; // into pointLights()
    std::vector<Draw> draws;
  };

  /** UI quads sharing a texture (null: none, white), drawn in one call. */
  struct UiBatch
  {
    const UiTexture* texture = nullptr;
    uint32_t first = 0;
    uint32_t count = 0;
  };

  /** Start a frame: forget the last one's draws. */
  void begin(const FrameParams& frame);
  bool open() const { return m_open; }

  /**
   * Add a mesh's draws. `hasBlend`: whether any section draws see-through
   * with the batch's materials (the backend knows its materials).
   */
  void addMesh(const MeshBatch& batch,
               uint32_t meshSlot,
               const PlannedMesh& mesh,
               bool hasBlend);
  void addGrass(const GrassBatch& batch);
  void addUi(const UiDrawList& list);

  /**
   * Work out the rest for a viewport: sort the see-through draws, pick the
   * shadowed lights and their casters, cluster the lights, fill the uniforms.
   * `meshes` looks a mesh's bounds up by slot (null: since destroyed).
   */
  void finish(glm::ivec2 viewport,
              uint32_t shadowMapResolution,
              uint32_t pointShadowResolution,
              const std::function<const PlannedMesh*(uint32_t)>& meshes);

  const FrameParams& frame() const { return m_frame; }
  const std::vector<MeshInstance>& instances() const { return m_instances; }
  const std::vector<uint32_t>& drawMaterials() const { return m_drawMaterials; }
  const std::vector<Draw>& litDraws() const { return m_litDraws; }
  /** Farthest first (after finish()). */
  const std::vector<Draw>& blendDraws() const { return m_blendOrder; }
  const std::vector<Draw>& shadowDraws() const { return m_shadowDraws; }
  /**
   * The opaque draws the camera sees that don't cast the sun's shadow (often
   * the ground): rain's overhead map draws them as well as shadowDraws(), so
   * rain lands on them too.
   */
  const std::vector<Draw>& unshadowedDraws() const { return m_unshadowedDraws; }
  /**
   * With cascades (frame().cascades.count > 1): each cascade's casters, those
   * of shadowDraws() that reach into it. Else empty.
   */
  const std::vector<Draw>& cascadeDraws(std::size_t cascade) const
  {
    return m_cascadeDraws[cascade];
  }
  /** The sun's shadow maps this frame: 1 (the one map) or its cascades. */
  uint32_t shadowCascades() const
  {
    return m_frame.cascades.count > 1 ? m_frame.cascades.count : 1;
  }
  const std::vector<ShadowedLight>& shadowedLights() const
  {
    return m_shadowedLights;
  }
  const std::vector<GrassBatch>& grassDraws() const { return m_grassDraws; }

  const FrameUniforms& frameUniforms() const { return m_uniforms; }
  /** The lit lights, at most kMaxPointLights. */
  const std::vector<GpuPointLight>& pointLights() const { return m_gpuLights; }
  const ClusterLights& clusters() const { return m_clusters; }
  /** Each shadowed light's six faces, in cube order. */
  const std::vector<FaceUniforms>& faces() const { return m_faces; }

  const std::vector<UiInstance>& uiInstances() const { return m_uiInstances; }
  const std::vector<UiBatch>& uiBatches() const { return m_uiBatches; }

  /** Bloom's uniforms for a pass reading a source of this size. */
  PostUniforms bloomUniforms(glm::ivec2 sourceSize) const;
  /**
   * The composite's: the glow added in (none when bloom is off), and the
   * frame's grading, for a target `viewport` big.
   */
  PostUniforms compositeUniforms(glm::ivec2 viewport) const;
  /** A physical sky's atmosphere, for its reflections' passes. */
  SkyUniforms skyUniforms() const;
  /**
   * Auto exposure's (ColorGrading::autoExposure), metering the scene at
   * `viewport`, `seconds` after the last frame's.
   */
  ExposureUniforms exposureUniforms(glm::ivec2 viewport, float seconds) const;
  /**
   * Ambient occlusion's passes', at `size` (half the viewport's), a blur
   * stepping `blurStep` texels (zero for the occlusion pass itself).
   */
  OcclusionUniforms occlusionUniforms(glm::ivec2 size,
                                      glm::ivec2 blurStep = {0, 0}) const;
  /**
   * Whether the frame has contact shadows: on, and lit by a physical sky
   * (they shade only its light).
   */
  bool contactShadowsOn() const;
  /**
   * Whether ambient occlusion's passes run: for ambient occlusion, contact
   * shadows or both.
   */
  bool screenOcclusionOn() const
  {
    return m_frame.ambientOcclusion.enabled || contactShadowsOn();
  }
  /**
   * Whether rain's overhead map is drawn this frame (rainMap(), from
   * shadowDraws()): while the weather has rain, wet or puddles, with the sun's
   * shadows on.
   */
  bool rainMapOn() const;
  /** Rain's overhead map this frame (after finish()). */
  const RainMap& rainMap() const { return m_rainMap; }
  /**
   * Rain's streaks, splashes and drips drawn this frame (after finish()).
   */
  uint32_t rainDrops() const
  {
    return static_cast<uint32_t>(m_uniforms.rainDepth.z +
                                 m_uniforms.rainDepth.w +
                                 m_uniforms.rainDrips.x);
  }

  /** Depth of field's, if the frame has it. */
  std::optional<FocusUniforms> focusUniforms(glm::ivec2 viewport) const;

private:
  bool m_open = false;
  FrameParams m_frame;
  std::vector<PointLight> m_pointLights;
  std::vector<MeshInstance> m_instances;
  std::vector<uint32_t> m_drawMaterials;
  std::vector<Draw> m_litDraws;
  struct BlendDraw
  {
    Draw draw;
    float distance = 0.0f; // from the camera
  };
  std::vector<BlendDraw> m_blendDraws;
  std::vector<Draw> m_blendOrder;
  std::vector<Draw> m_shadowDraws;
  std::vector<Draw> m_unshadowedDraws;
  std::array<std::vector<Draw>, kMaxShadowCascades> m_cascadeDraws;
  struct CasterBatch
  {
    Draw base;
    std::size_t first = 0;
    std::size_t count = 0;
  };
  std::vector<CasterBatch> m_casterBatches;
  std::vector<MeshInstance> m_casters;
  std::vector<ShadowedLight> m_shadowedLights;
  std::vector<GrassBatch> m_grassDraws;
  std::vector<UiQuad> m_uiQuads;

  FrameUniforms m_uniforms{};
  RainMap m_rainMap;
  std::vector<GpuPointLight> m_gpuLights;
  ClusterGrid m_grid;
  ClusterLights m_clusters;
  std::vector<FaceUniforms> m_faces;
  std::vector<UiInstance> m_uiInstances;
  std::vector<UiBatch> m_uiBatches;
};

/** A grass field as its backend holds it, for planning its growth. */
struct GrassFieldState
{
  GrassGrid groundGrid;
  float groundArea = 0.0f; // with ground, in its own XY's units
  GrassGrid mapsGrid;      // empty: unpainted
  glm::vec4 trampleBounds{0.0f, 0.0f, 1.0f, 1.0f}; // min xy, 1 / size
  std::array<uint32_t, 3> capacity{0, 0, 0};       // blades a list
  uint32_t shadowCapacity = 0; // blades that can cast the sun's shadow
};

/** How a grass field grows and draws this frame (see planGrass). */
struct GrassPlan
{
  GrassUniforms draw{};
  // Growing: nothing when no blades are in reach (its ground still draws).
  bool grows = false;
  GrassGrowUniforms grow{};
  // Blades each list should have room for; more than the field's capacity
  // means making its blade buffer again (grow.capacity is set from it).
  std::array<uint32_t, 3> capacity{0, 0, 0};
  // And for the blades casting the sun's shadow (their indices); likewise.
  uint32_t shadowCapacity = 0;
  glm::uvec3 workgroups{0, 0, 0};
};

/**
 * How far from the camera a grass field's blades cast the sun's shadow this
 * frame (0: they don't): the reach of the nearest kGrassShadowCascades
 * cascades (at most kGrassShadowMostReach), or with one shadow map,
 * kGrassShadowOneMapReach. Further out, a blade's shadow is less than a texel
 * of its map.
 */
float grassShadowReach(const GrassBatch& batch, const FrameParams& frame);

/**
 * The share of a grass field's blades kept at `distance` from the camera, as
 * its grow pass keeps them (`grassKept` in meshGrassGrow.wgsl, by
 * `uniforms.thinning` and the draw distance): 1 near the camera, falling
 * smoothly with distance, to 0 at the draw distance. Each blade has its own
 * rank, the same every frame (0..1, the coarser its lattice the lower), and
 * is kept where it's below this: so the blades that are kept are always the
 * same ones, wherever the camera is.
 */
float grassKeptShare(const GrassGrowUniforms& uniforms, float distance);

/**
 * Whether grass is thinned the old way (SFS_GRASS_LEGACY_THINNING set): each
 * of its lattices reaching so far from the camera, its blades shrinking away
 * at its end (for comparing).
 */
bool grassLegacyThinning();

/** How many of the sun's nearest cascades grass casts its shadow into. */
inline constexpr uint32_t kGrassShadowCascades = 2;
/** The furthest grass casts the sun's shadow, with cascades. */
inline constexpr float kGrassShadowMostReach = 32.0f;
/** How far grass casts the sun's shadow with one shadow map. */
inline constexpr float kGrassShadowOneMapReach = 8.0f;
/**
 * The lowest the sun (its direction's Z) can be for grass tiles that cast
 * shadows to be left out when hidden (their shadows, falling up to about 6
 * times their height away, allowed for).
 */
inline constexpr float kGrassLowestCulledSun = 0.16f;

/**
 * Where grass's ground carries on past its maps (meshGrass.wgsl's
 * fs_ground; a big mesh's grass has them only in a window around the
 * camera): over its terrain's (`terrain`, its ground's material) layers as
 * much as each is `meadow`, out to the horizon; none without one.
 */
void grassFarGround(GrassUniforms& uniforms, const TerrainMaterial* terrain);

/**
 * Plan a grass field's frame: its draw's uniforms, and (if any blades are in
 * reach) its growth's and the room its blade lists need. Nothing for a field
 * with no blades to grow (no density or height).
 */
std::optional<GrassPlan> planGrass(const GrassBatch& batch,
                                   const GrassFieldState& field,
                                   const FrameParams& frame);

} // namespace sfs
