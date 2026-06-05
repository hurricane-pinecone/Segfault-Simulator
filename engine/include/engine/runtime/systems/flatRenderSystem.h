#pragma once

#include "engine/core/ecs/system.h"
#include "engine/core/rendering/flatProjection.h"
#include "engine/runtime/rendering/flatRenderContext.h"
#include "engine/runtime/rendering/modules/renderModuleHost.h"
#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

class AssetStore;
class IQuadRenderer;

/**
 * Frame-global lighting for the flat 2D lit path. Sprites are lit by this plus
 * the scene's point lights (LightEmitterComponent). Defaults to fully lit with
 * no directional shading, so a game gets flat, unshaded sprites until it dials
 * in ambient/sun.
 */
struct FlatLighting
{
  float ambient = 1.0f;
  glm::vec3 ambientColor{1.0f, 1.0f, 1.0f};
  glm::vec3 sunDirection{0.0f, 0.0f, 1.0f};
  float diffuseStrength = 0.0f;
};

/**
 * Flat 2D render system: the general-purpose counterpart to
 * IsometricRenderSystem. Draws every SpriteComponent + TransformComponent entity
 * through the core IQuadRenderer as a lit screen-space quad, lit by the scene's
 * LightEmitterComponent point lights, ordered by RenderLayerComponent then world
 * Y via the depth buffer. Hosts generic render modules (e.g. Particles) through
 * the templated RenderModuleHost, exactly as the isometric system does, but with
 * no heightfield, projection-baked shaders, or dynamic_cast to an iso backend --
 * it needs only the core renderer.
 *
 * The game feeds it a FlatProjection each frame (from its camera) via
 * setProjection.
 */
class FlatRenderSystem : public System,
                         public RenderModuleHost<FlatRenderContext>
{
public:
  FlatRenderSystem(AssetStore& assetStore, IQuadRenderer& quadRenderer);

  // The world->screen projection for the frame (owned by the game/camera).
  void setProjection(const FlatProjection* projection)
  {
    m_projection = projection;
  }

  void setLighting(const FlatLighting& lighting) { m_lighting = lighting; }

protected:
  void create() override;
  void update(double deltaTime) override { updateModules(deltaTime); }
  void render() override;

  ModuleInit moduleInit() override { return {registry, &m_assetStore}; }

private:
  AssetStore& m_assetStore;
  IQuadRenderer& m_quadRenderer;

  const FlatProjection* m_projection = nullptr;
  FlatRenderContext m_context;
  FlatLighting m_lighting;
};

} // namespace sfs
