#pragma once

#include "engine/core/rendering/3d/mesh.h"
#include "engine/runtime/rendering3d/frame3D.h"
#include "glm/glm/ext/vector_int2.hpp"

#include <cstdint>

namespace sfs
{

/**
 * Backend-agnostic 3D renderer: the contract RenderSystem3D draws through, and
 * the seam a second graphics API plugs into (WebGpuRenderer3D today). The 3D
 * counterpart of IQuadRenderer.
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
  virtual void updateMaterial(MaterialHandle handle, const MeshMaterial& material) = 0;

  /** Free a material. Unknown or invalid handles are ignored. */
  virtual void destroyMaterial(MaterialHandle material) = 0;

  /** Size of the render target in pixels; zero while minimised. */
  virtual glm::ivec2 viewportSize() const = 0;

  /** Edge length of the square sun shadow map, in texels. */
  virtual uint32_t shadowMapResolution() const = 0;

  /** Start a frame. Draws issued before this are ignored. */
  virtual void beginFrame(const FrameParams3D& frame) = 0;

  /** Queue one mesh's instances. The spans need only live until the call
   * returns. */
  virtual void drawMesh(const MeshBatch3D& batch) = 0;

  /** Render everything queued since beginFrame(). */
  virtual void endFrame() = 0;
};

} // namespace sfs
