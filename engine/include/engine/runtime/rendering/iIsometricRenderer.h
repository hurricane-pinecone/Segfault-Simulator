#pragma once

#include "engine/core/rendering/quads.h"
#include "engine/core/rendering/vertices.h"
#include "engine/runtime/rendering/commands/commands.h"
#include "engine/runtime/rendering/iQuadRenderer.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include <cstddef>
#include <cstdint>

namespace sfs
{

/**
 * Sun-shadow sampling style for the block-geometry path.
 *
 * Smooth samples the linearly-filtered heightmap for soft, rounded shadow
 * edges. Sharp walks tiles for blocky, tile-aligned edges.
 */
enum class SunShadowStyle
{
  Smooth,
  Sharp,
};

/**
 * Per-frame projection and lighting inputs for the persistent decal pipeline.
 * Carries the isometric projection as plain scalars (so the renderer stays
 * backend-agnostic) plus the frame's depth-key range, letting the decal vertex
 * shader project world-space decals with clip-z matching the scene.
 *
 * @param tileWidth      tile width in pixels
 * @param tileHeight     tile height in pixels
 * @param worldScale     world-to-pixel scale factor
 * @param zoom           camera zoom
 * @param cameraIso      camera position in isometric pixel space
 * @param screenCenter   screen pixel that grid origin maps to
 * @param elevationStep  pixels of vertical rise per elevation level
 * @param depthMin       minimum painter sort-key this frame
 * @param depthInvRange  reciprocal of the frame's sort-key range (0 if
 * degenerate)
 * @param ambient        scene ambient level, so decals dim at night
 * @param ambientColor   scene ambient colour
 */
struct DecalFrameParams
{
  float tileWidth = 0.0f;
  float tileHeight = 0.0f;
  float worldScale = 1.0f;
  float zoom = 1.0f;
  glm::vec2 cameraIso{0.0f, 0.0f};
  glm::vec2 screenCenter{0.0f, 0.0f};
  float elevationStep = 8.0f;
  float depthMin = 0.0f;
  float depthInvRange = 0.0f;
  float ambient = 1.0f;
  glm::vec3 ambientColor{1.0f, 1.0f, 1.0f};
};

/**
 * Isometric-heightfield extension to the core 2D renderer. Adds the rendering
 * a 2.5D iso game needs on top of IQuadRenderer: an elevation heightmap (for
 * point-light and sun occlusion), real block-face geometry, the sun-shadow
 * style, the projected terrain/sprite shadow pipelines, and a persistent
 * world-projected decal store. A backend used by IsometricRenderSystem must
 * implement this interface.
 */
class IIsometricRenderer : public virtual IQuadRenderer
{
public:
  using IQuadRenderer::submit;

  /** Queue a merged surface mesh (water, lava, fog), projected and
   * depth-stamped. */
  virtual void submit(const SurfaceCommand& command) = 0;

  /**
   * Draw a lit, textured mesh of terrain block faces. Positions are screen
   * pixels; lighting uses the bound point lights, heightmap, and the sun set
   * via setGeometryLighting.
   *
   * @param vertices      triangle vertices (screen-pixel positions)
   * @param count         number of vertices
   * @param texture       material texture handle
   * @param surfaceEffect SurfaceEffect::Type as an int
   */
  virtual void drawGeometry(const GeometryVertex* vertices,
                            std::size_t count,
                            unsigned int texture,
                            int surfaceEffect) = 0;
  /**
   * Set the sun/ambient lighting applied to the geometry path for the frame.
   *
   * @param ambient         ambient light level
   * @param lightColor      sun colour
   * @param sunDirection    direction toward the sun (world space, z up)
   * @param diffuseStrength sun diffuse contribution
   */
  virtual void setGeometryLighting(float ambient,
                                   glm::vec3 lightColor,
                                   glm::vec3 sunDirection,
                                   float diffuseStrength) = 0;

  /**
   * Cutaway clip: the geometry pass drops terrain above `clipElevation` (the
   * roof) and, when `radius > 0`, outside `radius` of `center` (world tiles),
   * so only the cave around the player shows. A large `clipElevation` + `radius
   * <= 0` disables it. Default: no cut.
   */
  virtual void setGeometryClip(float /*clipElevation*/,
                               glm::vec2 /*center*/,
                               float /*radius*/)
  {
  }

  /** Select the sun-shadow sampling style for the heightmap march. */
  virtual void setSunShadowStyle(SunShadowStyle style) = 0;

  /**
   * Enable or disable the in-shader heightmap sun-shadow march (lit + geometry
   * pipelines). Disable when terrain shadows come from another technique (e.g.
   * the projected shadow system) to avoid double shadowing.
   */
  virtual void setSunShadowMarchEnabled(bool enabled) = 0;

  /** Queue a terrain shadow quad (stencil-merged so overlaps do not stack). */
  virtual void submitTerrainShadow(const Quad& command) = 0;
  /** Queue a projected sprite shadow, batched per shadow atlas texture. */
  virtual void submitSpriteShadow(const FreeformQuad& command) = 0;

  /**
   * Set the projection and depth range used to project persistent decals.
   * Call once per frame before drawing decals.
   */
  virtual void setDecalFrameParams(const DecalFrameParams& params) = 0;
  /**
   * Stamp this frame's new permanent splats into a paint target's texture,
   * creating the texture (texW x texH, cleared transparent) on first use. The
   * splats accumulate with premultiplied-alpha "over" blending, so painting the
   * same spot repeatedly never grows memory and a new colour paints over.
   *
   * @param key    paint target key (ground chunk or wall face)
   * @param texW   target texture width in texels
   * @param texH   target texture height in texels
   * @param sprite decal sprite texture sampled by each splat
   * @param verts  bake vertices ([0,1] target space + sprite uv + colour)
   * @param count  number of vertices
   */
  virtual void bakeDecals(std::int64_t key,
                          int texW,
                          int texH,
                          unsigned int sprite,
                          const DecalBakeVertex* verts,
                          std::size_t count) = 0;
  /**
   * Replace a paint target's persistent draw geometry: the world-space quads
   * that sample its paint texture (one per painted ground tile, or the wall
   * face). Sent only when the target's painted set changes.
   */
  virtual void uploadPaintDraw(std::int64_t key,
                               const DecalVertex* verts,
                               std::size_t count) = 0;
  /** Release a paint target's texture + draw buffer. */
  virtual void freePaintTarget(std::int64_t key) = 0;
  /** Draw a paint target's quads, sampling its baked paint texture. */
  virtual void drawPaintTarget(std::int64_t key) = 0;
  /**
   * Draw this frame's animating (unsettled) decals from a transient buffer.
   *
   * @param vertices animating decal vertices
   * @param count    number of vertices
   * @param texture  decal texture handle
   */
  virtual void drawDecalsDynamic(const DecalVertex* vertices,
                                 std::size_t count,
                                 unsigned int texture) = 0;

  /**
   * Upload the terrain elevation grid used to occlude point lights and the sun
   * against terrain.
   *
   * @param elevations  row-major grid of integer elevation levels from the
   * origin
   * @param width       grid width in tiles (<= 0 disables occlusion)
   * @param height      grid height in tiles
   * @param originX     world tile x of the grid origin
   * @param originY     world tile y of the grid origin
   * @param heightScale converts an emitter's height into elevation levels
   */
  virtual void setHeightmap(const int* elevations,
                            int width,
                            int height,
                            int originX,
                            int originY,
                            float heightScale) = 0;
};

} // namespace sfs
