#pragma once

#include "SDL2/SDL_pixels.h"
#include "SDL2/SDL_surface.h"
#include "engine/rendering/blendMode.h"
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/quads.h"
#include "engine/rendering/vertices/vertices.h"
#include "glm/glm/ext/vector_float2.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

namespace sfs
{

// Per-frame inputs for the persistent decal pipeline: enough of the isometric
// projection (kept as plain scalars so this stays backend/projection-agnostic)
// for the decal vertex shader to project world-space decals, plus the frame's
// depth-key range so its clip-z matches assignClipDepth's normalisation.
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

  // Scene lighting so decals respond to day/night + point lights (the point
  // light set itself is the one already bound via setPointLights). Without this
  // blood glows full-red in the dark.
  float ambient = 1.0f;
  glm::vec3 ambientColor{1.0f, 1.0f, 1.0f};
};

// Backend-agnostic quad renderer interface implemented by concrete backends.
class IQuadRenderer
{
public:
  virtual ~IQuadRenderer() = default;

  virtual void initialize() = 0;
  virtual void shutdown() = 0;

  virtual unsigned int getOrCreateTexture(const std::string& textureId,
                                          SDL_Surface* surface) = 0;
  virtual unsigned int uploadSurfaceTexture(SDL_Surface* surface) = 0;
  virtual void deleteTexture(unsigned int texture) = 0;

  virtual void submit(const Quad& command) = 0;
  virtual void submit(const TexturedQuad& command) = 0;
  virtual void submit(const FreeformQuad& command) = 0;
  virtual void submit(const LitQuad& command) = 0;

  // Submit a whole batch of lit quads that share one material + lighting (so
  // the batch key is set once, not rebuilt per quad).
  virtual void submitLitBatch(const LitQuadBatch& batch,
                              unsigned int texture,
                              unsigned int normalTexture,
                              bool hasNormalMap,
                              int surfaceEffect) = 0;

  virtual void submit(const SurfaceCommand& command) = 0;
  virtual void submitTerrainShadow(const Quad& command) = 0;

  // Projected sprite shadows, batched by texture (one draw per shadow atlas).
  virtual void submitSpriteShadow(const FreeformQuad& command) = 0;

  // A batch of unlit particle billboards sharing one texture + blend mode, drawn
  // in one call. Never depth-writes. depthTested = true (world particles) tests
  // against the scene depth so terrain occludes them; false (screen-space
  // overlays) ignores depth and always draws on top.
  virtual void submitParticleBatch(const ParticleBatch& batch,
                                   unsigned int texture,
                                   BlendMode blend,
                                   bool depthTested) = 0;

  // --- Persistent decals (stains) ---
  // Decals are stored world-space in per-chunk GPU buffers and projected in the
  // decal vertex shader, so settled ones are never rebuilt per frame.
  //
  // Set once per frame (projection + depth range) before drawing decals.
  virtual void setDecalFrameParams(const DecalFrameParams& params) = 0;
  // Replace a chunk's persistent buffer (called only when its settled set changes).
  virtual void uploadDecalChunk(std::int64_t key,
                                const DecalVertex* vertices,
                                std::size_t count) = 0;
  // Append new vertices to a chunk's persistent buffer (grows it as needed) so
  // adding decals is O(new), not O(chunk total) -- keeps sustained painting flat.
  virtual void appendDecalChunk(std::int64_t key,
                                const DecalVertex* vertices,
                                std::size_t count) = 0;
  virtual void freeDecalChunk(std::int64_t key) = 0;
  // Draw a chunk's persistent buffer / the per-frame animating decals with the
  // given texture. Depth-tested against the scene, no depth write, alpha blended.
  virtual void drawDecalChunk(std::int64_t key, unsigned int texture) = 0;
  virtual void drawDecalsDynamic(const DecalVertex* vertices,
                                 std::size_t count,
                                 unsigned int texture) = 0;

  // Immediate-mode draw (text, UI, simple sprites), bypassing the batch queue.
  virtual void drawImmediate(const TexturedQuad& command) = 0;

  virtual void begin() = 0;
  virtual void flush() = 0;

  virtual void
  drawLineLoop(const glm::vec2* points, int count, SDL_Color color) = 0;

  virtual void setViewportSize(int width, int height) = 0;
  virtual void setSurfaceTime(float time) = 0;

  // The frame's point lights, bound to every subsequent lit/surface draw.
  virtual void setPointLights(const PointLightSet& lights) = 0;

  // Terrain elevation grid (one int level per tile, row-major from origin) used
  // to occlude point lights against terrain. heightScale converts a light's
  // emitter height into elevation levels. width <= 0 disables occlusion.
  virtual void setHeightmap(const int* elevations,
                            int width,
                            int height,
                            int originX,
                            int originY,
                            float heightScale) = 0;
};

} // namespace sfs
