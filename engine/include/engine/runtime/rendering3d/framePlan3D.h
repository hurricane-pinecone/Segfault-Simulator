#pragma once

#include "engine/core/rendering/3d/grass.h"
#include "engine/core/rendering/3d/lightClusters.h"
#include "engine/core/rendering/3d/mesh.h"
#include "engine/runtime/rendering3d/frame3D.h"
#include "engine/runtime/rendering3d/uiDraw.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include "glm/glm/ext/vector_int4.hpp"
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
 * Most point lights with shadows at once (ShadowSettings3D::pointLights is
 * capped to it): six cube faces each fit a texture array's 256 layers.
 */
inline constexpr uint32_t kMaxShadowedPointLights = 32;

// The engine's shaders' uniforms and buffer elements, laid out as the WGSL
// declares them (16-byte aligned), for every backend to upload as they are.

/** `Frame` in mesh3DCommon.wgsl. */
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
  glm::vec4 clusterDepth;  // near, far, slices per log(depth / near), tile px
  glm::vec4 wind;          // which way it blows (unit xy), strength, gustiness
  glm::vec4 haze;          // strength, 1 / its half distance
};
static_assert(sizeof(FrameUniforms) == 64 * 2 + 16 * 11,
              "FrameUniforms must match the WGSL Frame layout");

/** `PointLight` in mesh3DLit.wgsl (a storage buffer's elements). */
struct GpuPointLight
{
  glm::vec4 positionRadius;
  glm::vec4 colorIntensity;
  glm::vec4 shadow; // its cube's layer (-1: none), 1 / its face size, its size
};

/** `ShadowFace` in mesh3DPointShadow.wgsl. */
struct FaceUniforms
{
  glm::mat4 viewProjection;
  glm::vec4 light; // position, 1 / reach
};

/** `Grass` in mesh3DGrass.wgsl. */
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

/** `Grow` in mesh3DGrassGrow.wgsl. */
struct GrassGrowUniforms
{
  glm::mat4 model;
  glm::mat4 normalMatrix;
  std::array<glm::vec4, 6> planes;
  glm::vec4 camera; // xyz, draw distance
  glm::vec4 ground; // the ground grid's min, texel size
  glm::ivec4 groundSize;
  glm::vec4 maps;                     // the maps' grid min, 1 / its size
  glm::vec4 shape;                    // height, variation, width, finest cell
  glm::vec4 reach;                    // each lattice's
  glm::vec4 detail;                   // 5 segments to, 2 to
  glm::uvec4 capacity;                // each list's
  std::array<glm::ivec4, 4> lattices; // first cell, count
};
static_assert(sizeof(GrassGrowUniforms) == 416,
              "GrassGrowUniforms must match mesh3DGrassGrow.wgsl's Grow");

/** A grown blade (mesh3DGrassGrow.wgsl's `GrownBlade`), in bytes. */
inline constexpr uint32_t kGrownBladeBytes = 32;

/** `QuadIn` in ui3D.wgsl: a UiQuad as the GPU reads it. */
struct UiInstance
{
  glm::vec4 rect;
  glm::vec4 uv;
  glm::vec4 color;
  glm::vec4 borderColor;
  glm::vec4 shape; // radius, softness, border, kind
};

/** `Focus` in post3DFocus.wgsl. */
struct FocusUniforms
{
  glm::vec4 depth; // projection[2][2], projection[3][2], perspective (1/0)
  glm::vec4 focus; // distance, half the sharp range, blur radius (texels)
  glm::vec4 texel; // a half-size texel (uv)
};

/** `Post` in post3D.wgsl. */
struct PostUniforms
{
  glm::vec4 texel; // xy the source's texel size
  glm::vec4 bloom; // threshold, softness, radius, intensity
};

/** `Material` in mesh3DCommon.wgsl: the standard surface's parameters. */
struct MaterialUniforms
{
  glm::vec4 baseColor;
  glm::vec4 params;      // normal scale, metallic, roughness, alpha cutoff
  glm::vec4 uvTransform; // tiling xy, offset zw
  glm::uvec4 mode;       // x: AlphaMode, y: unlit
};

/** A standard-surface material's uniforms. */
MaterialUniforms standardMaterialUniforms(const MeshMaterial& material);

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
 * A 3D frame worked out on the CPU, the same for every backend: which
 * instances draw in which pass (lit, see-through back to front, the sun's
 * shadow, each shadowed point light's), the point lights and the clusters
 * they light, and every pass's uniforms. A backend feeds it the frame's draws
 * (IRenderer3D's), then uploads what it worked out and records its passes.
 */
class FramePlan3D
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
  void begin(const FrameParams3D& frame);
  bool open() const { return m_open; }

  /**
   * Add a mesh's draws. `hasBlend`: whether any section draws see-through
   * with the batch's materials (the backend knows its materials).
   */
  void addMesh(const MeshBatch3D& batch,
               uint32_t meshSlot,
               const PlannedMesh& mesh,
               bool hasBlend);
  void addGrass(const GrassBatch3D& batch);
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

  const FrameParams3D& frame() const { return m_frame; }
  const std::vector<MeshInstance>& instances() const { return m_instances; }
  const std::vector<uint32_t>& drawMaterials() const { return m_drawMaterials; }
  const std::vector<Draw>& litDraws() const { return m_litDraws; }
  /** Farthest first (after finish()). */
  const std::vector<Draw>& blendDraws() const { return m_blendOrder; }
  const std::vector<Draw>& shadowDraws() const { return m_shadowDraws; }
  const std::vector<ShadowedLight>& shadowedLights() const
  {
    return m_shadowedLights;
  }
  const std::vector<GrassBatch3D>& grassDraws() const { return m_grassDraws; }

  const FrameUniforms& frameUniforms() const { return m_uniforms; }
  /** The lit lights, at most kMaxPointLights3D. */
  const std::vector<GpuPointLight>& pointLights() const { return m_gpuLights; }
  const ClusterLights& clusters() const { return m_clusters; }
  /** Each shadowed light's six faces, in cube order. */
  const std::vector<FaceUniforms>& faces() const { return m_faces; }

  const std::vector<UiInstance>& uiInstances() const { return m_uiInstances; }
  const std::vector<UiBatch>& uiBatches() const { return m_uiBatches; }

  /** Bloom's uniforms for a pass reading a source of this size. */
  PostUniforms bloomUniforms(glm::ivec2 sourceSize) const;
  /** The composite's: the glow added in (none when bloom is off). */
  PostUniforms compositeUniforms() const;
  /** Depth of field's, if the frame has it. */
  std::optional<FocusUniforms> focusUniforms(glm::ivec2 viewport) const;

private:
  bool m_open = false;
  FrameParams3D m_frame;
  std::vector<PointLight3D> m_pointLights;
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
  struct CasterBatch
  {
    Draw base;
    std::size_t first = 0;
    std::size_t count = 0;
  };
  std::vector<CasterBatch> m_casterBatches;
  std::vector<MeshInstance> m_casters;
  std::vector<ShadowedLight> m_shadowedLights;
  std::vector<GrassBatch3D> m_grassDraws;
  std::vector<UiQuad> m_uiQuads;

  FrameUniforms m_uniforms{};
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
  glm::uvec3 workgroups{0, 0, 0};
};

/**
 * Plan a grass field's frame: its draw's uniforms, and (if any blades are in
 * reach) its growth's and the room its blade lists need. Nothing for a field
 * with no blades to grow (no density or height).
 */
std::optional<GrassPlan> planGrass(const GrassBatch3D& batch,
                                   const GrassFieldState& field,
                                   const FrameParams3D& frame);

} // namespace sfs
