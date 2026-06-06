#pragma once

#include "engine/core/ecs/system.h"
#include "engine/core/rendering/projection/flatProjection.h"
#include "engine/runtime/assetStore/sprite.h"
#include "engine/runtime/rendering/flatDecal.h"
#include "engine/runtime/rendering/flatRenderContext.h"
#include "engine/runtime/rendering/modules/renderModuleHost.h"
#include "glm/glm/ext/vector_float3.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

namespace sfs
{

class AssetStore;
class IQuadRenderer;
class FlatDecalSink;

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
 * IsometricRenderSystem. Draws every SpriteComponent + TransformComponent
 * entity through the core IQuadRenderer as a lit screen-space quad, lit by the
 * scene's LightEmitterComponent point lights, ordered by RenderLayerComponent
 * then world Y via the depth buffer. Hosts generic render modules (e.g.
 * Particles) through the templated RenderModuleHost, exactly as the isometric
 * system does, but with no heightfield, projection-baked shaders, or
 * dynamic_cast to an iso backend -- it needs only the core renderer.
 *
 * The game feeds it a FlatProjection each frame (from its camera) via
 * setProjection.
 */
class FlatRenderSystem : public System,
                         public RenderModuleHost<FlatRenderContext>
{
public:
  FlatRenderSystem(AssetStore& assetStore, IQuadRenderer& quadRenderer);
  ~FlatRenderSystem();

  // The world->screen projection for the frame (owned by the game/camera).
  void setProjection(const FlatProjection* projection)
  {
    m_projection = projection;
  }

  void setLighting(const FlatLighting& lighting) { m_lighting = lighting; }

  // Camera focus in world units (usually the followed entity). When more lights
  // exist than the shader can hold, the nearest to this point are kept.
  void setFocus(const glm::vec2& focus) { m_focus = focus; }

  // Stamp a persistent decal (blood, scorch, ...). It is drawn each frame,
  // depth-sorted with sprites by layer, until it expires. Any system can stamp
  // one -- it isn't an entity. Spatially coverage-limited: once a small world
  // cell holds `decalsPerCell` permanent decals it stops accepting more there,
  // so a hammered spot SATURATES and stays put instead of churning the buffer
  // (which would erase old marks as new ones land). Over the global cap the
  // oldest is still dropped as a backstop.
  void stampDecal(const FlatDecal& decal);

  // The sprites stamped decals use: a soft one for round drops and a hard one
  // for crisp directional streaks (pass the same id twice for a single look).
  // Sets up the decal sink returned by decalSink(), so a game's particle module
  // can enableStains() against this system.
  void setDecalSprites(SpriteId soft, SpriteId streak, int layer = 1);

  // The flat decal sink (built from setDecalSprites). Particles pull this in
  // enableStains() so a game never wires the sink by hand. Null until sprites
  // are set.
  IDecalSink* decalSink() override;

  // Max live decals before the oldest is evicted (bounds memory/frame cost).
  void setMaxDecals(int max) { m_maxDecals = max; }

  // Spatial saturation: cell size (world units) and how many permanent decals
  // one cell holds before further stamps there are dropped.
  void setDecalCoverage(float cellSize, int decalsPerCell)
  {
    m_decalCellSize = cellSize > 1.0f ? cellSize : 1.0f;
    m_decalsPerCell = decalsPerCell;
  }

protected:
  void create() override;
  void update(double deltaTime) override
  {
    updateModules(deltaTime);
    ageDecals(static_cast<float>(deltaTime));
  }
  void render() override;

  ModuleInit moduleInit() override { return {registry, &m_assetStore}; }

private:
  // Advance decals: follow their target, grow, and expire.
  void ageDecals(float deltaTime);

  AssetStore& m_assetStore;
  IQuadRenderer& m_quadRenderer;

  const FlatProjection* m_projection = nullptr;
  FlatRenderContext m_context;
  FlatLighting m_lighting;
  glm::vec2 m_focus{0.0f, 0.0f};

  // World cell key for the coverage map (packs floor(p / cellSize)).
  long long decalCell(const glm::vec2& worldPos) const;

  // The decal sink wrapping this system, built on demand by setDecalSprites /
  // decalSink(). Owned here so it outlives any particle module pointing at it.
  std::unique_ptr<FlatDecalSink> m_decalSink;
  SpriteId m_decalSoft = 0;
  SpriteId m_decalStreak = 0;
  int m_decalLayer = 1;

  std::vector<FlatDecal> m_decals;
  int m_maxDecals = 4000;

  // Coverage: live permanent-decal count per world cell (see stampDecal). A
  // permanent decal (lifetime < 0) counts; fading decals are ignored.
  std::unordered_map<long long, int> m_decalCoverage;
  float m_decalCellSize = 14.0f;
  int m_decalsPerCell = 10;
};

} // namespace sfs
