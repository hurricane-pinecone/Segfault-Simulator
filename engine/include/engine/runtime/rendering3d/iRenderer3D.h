#pragma once

#include "engine/core/rendering/3d/mesh.h"
#include "engine/runtime/rendering3d/frame3D.h"
#include "engine/runtime/rendering3d/uiDraw.h"
#include "glm/glm/ext/vector_int2.hpp"

#include <cstdint>

namespace sfs
{

/**
 * What a renderer can draw, for a backend that can't do everything (e.g. one
 * without compute shaders). Render modules and the editor skip what it lacks
 * rather than ask for it; the renderer ignores such draws anyway.
 */
struct RendererCapabilities
{
  bool grass = true;             // drawGrass (grown by a compute pass)
  bool materialGraphs = true;    // materials with a compiled graph shader
  bool pointLightShadows = true; // PointLight3D::castsShadow
  bool postEffects = true;       // bloom
  bool depthOfField = true;      // DepthOfFieldSettings3D
  bool antiAliasing = true;      // multisampled edges
};

/**
 * Backend-agnostic renderer: the contract RenderSystem3D draws through (for
 * flat, isometric and 3D games alike), and the seam a second graphics API
 * plugs into (WebGpuRenderer3D today).
 *
 * A frame is beginFrame(), any number of drawMesh(), then endFrame(), which
 * renders the sun's shadow map and the lit scene into the backend's current
 * target.
 */
class IRenderer3D
{
public:
  virtual ~IRenderer3D() = default;

  /**
   * Upload a mesh to the GPU.
   *
   * @return a handle for drawing it, or an invalid handle if the mesh is empty
   * or the upload failed.
   */
  virtual MeshHandle createMesh(const MeshData& mesh) = 0;

  /** Free a mesh. Unknown or invalid handles are ignored. */
  virtual void destroyMesh(MeshHandle mesh) = 0;

  /**
   * Upload a material (and its textures) for objects to use in place of a
   * mesh section's own (see MeshBatch3D::materials).
   *
   * @return a handle, or an invalid handle if the upload failed.
   */
  virtual MaterialHandle createMaterial(const MeshMaterial& material) = 0;

  /** Change a material in place: everything drawn with it changes. */
  virtual void updateMaterial(MaterialHandle handle,
                              const MeshMaterial& material) = 0;

  /** Free a material. Unknown or invalid handles are ignored. */
  virtual void destroyMaterial(MaterialHandle material) = 0;

  /**
   * Upload the ground a grass field grows on (see grass.h), to draw with
   * drawGrass(). Its maps start unpainted, and nothing's flattened.
   * @return a handle, or an invalid one if the ground is empty or the upload
   * failed.
   */
  virtual GrassHandle createGrass(const GrassGround& ground) = 0;

  /**
   * Part of the field's maps changed (or all of them, over a new grid: then
   * `image` covers all of it).
   */
  virtual void updateGrassMaps(GrassHandle grass,
                               const GrassMapsImage& image) = 0;

  /** Where the field's grass is flattened now (until the next update). */
  virtual void updateGrassTrample(GrassHandle grass,
                                  const GrassTrampleImage& image) = 0;

  /** Free a grass field. Unknown or invalid handles are ignored. */
  virtual void destroyGrass(GrassHandle grass) = 0;

  /** What it can draw. Everything, unless a backend says otherwise. */
  virtual RendererCapabilities capabilities() const { return {}; }

  /** Size of the render target in pixels; zero while minimised. */
  virtual glm::ivec2 viewportSize() const = 0;

  /** Edge length of the square sun shadow map, in texels. */
  virtual uint32_t shadowMapResolution() const = 0;

  /** Start a frame. Draws issued before this are ignored. */
  virtual void beginFrame(const FrameParams3D& frame) = 0;

  /** Queue one mesh's instances. The spans need only live until the call
   * returns. */
  virtual void drawMesh(const MeshBatch3D& batch) = 0;

  /** Queue a grass field (its blades in view, thinning with distance). */
  virtual void drawGrass(const GrassBatch3D& batch) = 0;

  /**
   * Queue UI to draw over the finished frame (after bloom and depth of
   * field), back to front. Its textures must live until endFrame().
   */
  virtual void drawUi(const UiDrawList& list) = 0;

  /** Render everything queued since beginFrame(). */
  virtual void endFrame() = 0;
};

} // namespace sfs
