#pragma once

#include "SDL2/SDL_pixels.h"
#include "SDL2/SDL_surface.h"
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/quads.h"
#include "glm/glm/ext/vector_float2.hpp"
#include <string>

namespace sfs
{

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
};

} // namespace sfs
