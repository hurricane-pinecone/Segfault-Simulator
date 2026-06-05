#pragma once

#include "engine/runtime/rendering/commands/commands.h"
#include "engine/runtime/rendering/iQuadRenderer.h"
#include "engine/runtime/rendering/quads.h"
#include "engine/runtime/rendering/vertices/vertices.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include <cstddef>
#include <cstdint>

namespace sfs
{

/**
 * Sun-shadow sampling style for the block-geometry path.
 *
 * Smooth samples the linearly-filtered heightmap for soft, rounded shadow edges.
 * Sharp walks tiles for blocky, tile-aligned edges.
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
 * @param depthInvRange  reciprocal of the frame's sort-key range (0 if degenerate)
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

  /** Queue a merged surface mesh (water, lava, fog), projected and depth-stamped. */
  virtual void submit(const SurfaceCommand& command) = 0;

  /**
   * Draw a lit, textured mesh of terrain block faces. Positions are screen
   * pixels; lighting uses the bound point lights, heightmap, and the sun set via
   * setGeometryLighting.
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
   * Replace a decal chunk's persistent GPU buffer.
   *
   * @param key      chunk key
   * @param vertices full vertex set for the chunk
   * @param count    number of vertices
   */
  virtual void uploadDecalChunk(std::int64_t key,
                                const DecalVertex* vertices,
                                std::size_t count) = 0;
  /**
   * Append vertices to a decal chunk's persistent buffer, growing it as needed,
   * so adding decals costs O(new) rather than O(chunk total).
   *
   * @param key      chunk key
   * @param vertices vertices to append
   * @param count    number of vertices
   */
  virtual void appendDecalChunk(std::int64_t key,
                                const DecalVertex* vertices,
                                std::size_t count) = 0;
  /** Release a decal chunk's persistent buffer. */
  virtual void freeDecalChunk(std::int64_t key) = 0;
  /** Draw a decal chunk's persistent buffer with the given texture. */
  virtual void drawDecalChunk(std::int64_t key, unsigned int texture) = 0;
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
   * @param elevations  row-major grid of integer elevation levels from the origin
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
