#pragma once

#include "engine/runtime/rendering/iQuadRenderer.h"

namespace sfs
{

// A do-nothing IQuadRenderer. The WebGPU backend draws everything through its
// own raymarch passes and has no GL quad pipeline, but Scene's SceneServices
// require a renderer reference; this satisfies that contract for scenes that
// never submit through it.
class NullQuadRenderer : public IQuadRenderer
{
public:
  bool initialize() override { return true; }
  void shutdown() override {}

  unsigned int getOrCreateTexture(const std::string&, SDL_Surface*) override
  {
    return 0;
  }
  unsigned int uploadSurfaceTexture(SDL_Surface*) override { return 0; }
  void deleteTexture(unsigned int) override {}

  void submit(const Quad&) override {}
  void submit(const TexturedQuad&) override {}
  void submit(const FreeformQuad&) override {}
  void submit(const LitQuad&) override {}

  void submitLitBatch(const LitQuadBatch&,
                      unsigned int,
                      unsigned int,
                      bool,
                      int) override
  {
  }
  void submitParticleBatch(const ParticleBatch&,
                           unsigned int,
                           BlendMode,
                           bool) override
  {
  }

  void drawImmediate(const TexturedQuad&) override {}

  void begin() override {}
  void flush() override {}

  void drawLineLoop(const glm::vec2*, int, SDL_Color) override {}

  void setViewportSize(int, int) override {}
  void setSurfaceTime(float) override {}
  void setPointLights(const PointLightSet&) override {}
};

} // namespace sfs
