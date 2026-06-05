#pragma once

#include "SDL2/SDL_pixels.h"
#include "SDL2/SDL_surface.h"
#include "engine/core/types/blendMode.h"
#include "engine/runtime/rendering/quads.h"
#include "glm/glm/ext/vector_float2.hpp"
#include <cstddef>
#include <string>

namespace sfs
{

/**
 * Backend-agnostic 2D quad renderer: the core rendering contract a render
 * system draws through. Provides texture management, batched and immediate quad
 * submission, lighting, and frame lifecycle. A render system is any sfs::System
 * that holds an IQuadRenderer& and issues draws between begin() and flush().
 *
 * Isometric-heightfield capabilities (block geometry, sun shadows, projected
 * shadow pipelines, world-projected decals, the elevation heightmap) are
 * defined by the IIsometricRenderer extension.
 */
class IQuadRenderer
{
public:
  virtual ~IQuadRenderer() = default;

  /**
   * Create GPU resources (shaders, buffers). Call once before any draw.
   *
   * @return true if the renderer is ready to draw; false if a resource (e.g. a
   * shader program) failed to build, leaving the renderer unusable.
   */
  virtual bool initialize() = 0;
  /** Release all GPU resources. */
  virtual void shutdown() = 0;

  /**
   * Return the GPU texture for an asset, uploading it on first use and caching
   * it under the given id.
   *
   * @param textureId cache key for the texture
   * @param surface   pixel source to upload if not already cached
   * @return GPU texture handle (0 on failure)
   */
  virtual unsigned int getOrCreateTexture(const std::string& textureId,
                                          SDL_Surface* surface) = 0;
  /**
   * Upload a surface to a new, uncached GPU texture owned by the caller.
   *
   * @param surface pixel source to upload
   * @return GPU texture handle (0 on failure)
   */
  virtual unsigned int uploadSurfaceTexture(SDL_Surface* surface) = 0;
  /** Delete a GPU texture previously returned by this renderer. */
  virtual void deleteTexture(unsigned int texture) = 0;

  /** Queue a solid-colour quad. */
  virtual void submit(const Quad& command) = 0;
  /** Queue a textured quad. */
  virtual void submit(const TexturedQuad& command) = 0;
  /** Queue a free-form (arbitrary 4-corner) textured quad. */
  virtual void submit(const FreeformQuad& command) = 0;
  /** Queue a lit quad (per-pixel lighting via the bound point lights). */
  virtual void submit(const LitQuad& command) = 0;

  /**
   * Queue a batch of lit quads sharing one material and lighting state, so the
   * batch key is set once rather than per quad.
   *
   * @param batch         the quads to draw
   * @param texture       material albedo texture handle
   * @param normalTexture material normal-map handle (ignored if hasNormalMap is
   * false)
   * @param hasNormalMap  whether normalTexture is valid
   * @param surfaceEffect SurfaceEffect::Type as an int, applied across the
   * batch
   */
  virtual void submitLitBatch(const LitQuadBatch& batch,
                              unsigned int texture,
                              unsigned int normalTexture,
                              bool hasNormalMap,
                              int surfaceEffect) = 0;

  /**
   * Queue a batch of unlit particle billboards sharing one texture and blend
   * mode, drawn in a single call and never writing depth.
   *
   * @param batch       the particle quads
   * @param texture     shared texture handle
   * @param blend       blend mode for the batch
   * @param depthTested true tests against scene depth (world particles occlude
   *                    behind terrain); false ignores depth (screen-space
   * overlays)
   */
  virtual void submitParticleBatch(const ParticleBatch& batch,
                                   unsigned int texture,
                                   BlendMode blend,
                                   bool depthTested) = 0;

  /** Draw a textured quad immediately (text, UI), bypassing the batch queue. */
  virtual void drawImmediate(const TexturedQuad& command) = 0;

  /** Begin a frame: reset pipeline state and clear per-frame batches. */
  virtual void begin() = 0;
  /** Flush all queued batches to the GPU, ending the frame's draws. */
  virtual void flush() = 0;

  /**
   * Draw a closed line loop in screen pixels (debug overlays).
   *
   * @param points screen-space points
   * @param count  number of points
   * @param color  line colour
   */
  virtual void
  drawLineLoop(const glm::vec2* points, int count, SDL_Color color) = 0;

  /**
   * Set the output resolution used to map pixel coordinates to NDC.
   *
   * @param width  viewport width in pixels
   * @param height viewport height in pixels
   */
  virtual void setViewportSize(int width, int height) = 0;
  /** Set the animation time (seconds) fed to time-driven surface shaders. */
  virtual void setSurfaceTime(float time) = 0;

  /** Bind the frame's point lights, applied to every subsequent lit draw. */
  virtual void setPointLights(const PointLightSet& lights) = 0;
};

} // namespace sfs
