#pragma once

#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep
#include "engine/core/rendering/iTerrainHeightSource.h"
#include "engine/core/rendering/renderQueue.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering/commands/commands.h"
#include "engine/runtime/rendering/iIsometricRenderer.h"
#include "engine/runtime/rendering/isometricRenderContext.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/modules/renderModuleHost.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_int2.hpp"

#include <SDL_pixels.h>
#include <SDL_rect.h>
#include <functional>
#include <typeindex>
#include <vector>

namespace sfs
{

struct WallFaceKey
{
  glm::ivec2 tile{0, 0};
  int side = 0;

  bool operator==(const WallFaceKey& other) const noexcept
  {
    return tile == other.tile && side == other.side;
  }
};

struct WallFaceKeyHash
{
  std::size_t operator()(const WallFaceKey& k) const noexcept
  {
    const std::uint32_t x = static_cast<std::uint32_t>(k.tile.x);
    const std::uint32_t y = static_cast<std::uint32_t>(k.tile.y);
    const std::uint32_t s = static_cast<std::uint32_t>(k.side);

    return static_cast<std::size_t>(x * 73856093u ^ y * 19349663u ^
                                    s * 83492791u);
  }
};

class IsometricRenderSystem : public System,
                              public RenderModuleHost<IsometricRenderContext>
{
public:
  IsometricRenderSystem(AssetStore& assetStore, IQuadRenderer& quadRenderer);

  ~IsometricRenderSystem();

  void setProjection(const IsometricProjection* projection);

  // Authoritative terrain heights for the point-light occlusion heightmap. When
  // set, the heightmap samples this instead of the per-tile ECS entities, so
  // its window is hole-free even while terrain streams in (see
  // ITerrainHeightSource).
  void setTerrainHeightSource(const ITerrainHeightSource* source);

  void setWaveTime(float time);
  void setWaveEnabled(bool enabled);

  /** Select the sun-shadow sampling style for the heightmap march
   * (smooth/sharp). */
  void setSunShadowStyle(SunShadowStyle style);

  /** The current sun-shadow sampling style. */
  SunShadowStyle sunShadowStyle() const { return m_sunShadowStyle; }

  // Module composition (withModule/withModules/hasModule/module/removeModule)
  // is inherited from RenderModuleHost<IsometricRenderContext>.

  // The persistent terrain-stain sink (IsometricDecalSink), registering it on
  // first use so a game enabling particle stains never wires decals by hand.
  IDecalSink* decalSink() override;

  /**
   * Visit each registered module with its type and the current render context,
   * for debug/editor UIs that read module settings(). Cross-cutting context
   * flags (e.g. geometryActive) are refreshed first so a module returns
   * settings appropriate to the active render mode.
   */
  void forEachModule(
      const std::function<
          void(std::type_index, Module&, const IsometricRenderContext&)>& fn);

  void drawDebugTile(const glm::vec2& gridPosition,
                     SDL_Color color = SDL_Color{255, 255, 0, 255});

  void drawDebugTile(const glm::vec2& gridPosition,
                     int elevation,
                     SDL_Color color = SDL_Color{255, 255, 0, 255});

  // Debug overlay for tuning collider boxes: WorldColliders as a ground-plane
  // diamond (world AABB projected at the entity's elevation), ScreenSpace
  // colliders as a 1px box over the sprite in screen space.
  void drawDebugColliders(SDL_Color worldColor = SDL_Color{0, 255, 0, 255},
                          SDL_Color screenColor = SDL_Color{255, 0, 0, 255});

  void markTerrainDirty();

  /** Set the scene's sun/sky ambient lighting (e.g. driven by a day/night
   * cycle). */
  void setAmbientLighting(const IsometricAmbientLighting& ambient);

  /**
   * Cutaway: when `active`, the geometry pass hides terrain above `level` (the
   * roof) and outside `radius` world-tiles of `center` (the surrounding rock +
   * far world), so a cave the player has entered shows alone. The scene drives
   * this from the player each frame.
   */
  void setGeometryClip(float level, glm::vec2 center, float radius, bool active)
  {
    m_geomClipElevation = level;
    m_geomClipCenter = center;
    m_geomClipRadius = radius;
    m_geomClipActive = active;
  }

  glm::vec2 screenToWorld(const glm::vec2& screenPosition,
                          float elevation = 0.0f) const;

  TilePick pickTile(const glm::vec2& screenPosition) const;

  glm::ivec2 screenToTile(const glm::vec2& screenPosition) const;

  IsometricRenderSystem(const IsometricRenderSystem&) = delete;
  IsometricRenderSystem& operator=(const IsometricRenderSystem&) = delete;

protected:
  void create() override;
  void update(double deltaTime) override;
  void render() override;

private:
  void beginBatches();

  // Rebuild m_pointLights from the scene's LightEmitterComponent entities.
  void gatherPointLights();

  // Eases each actor's ground elevation (m_actorElevation) toward the tile it
  // stands on, so a carried point light's occlusion ramps across an elevation
  // step instead of snapping. The sprite itself uses the discrete elevation
  // (snaps), so only the light reads this eased value.
  void updateActorElevations(double deltaTime);

  void flushBatches();

  unsigned int resolveTexture(const std::string* textureId);

  bool isTileEntity(const Entity& entity) const;

  // Discrete elevation (levels) of a terrain tile entity, for its render height
  // and depth. Actors derive theirs from the tile they stand on instead.
  float tileElevationLevel(const Entity& entity) const;

  // Terrain elevation (levels) the actor stands on, driven by its
  // FootColliderComponent footprint (falls back to the tile under its position
  // when it has none).
  float actorStandingElevation(const Entity& entity,
                               const TransformComponent& transform) const;

  bool tryGetTileElevationAt(const glm::vec2& position,
                             int& outElevation) const;

  int getTileElevationAt(const glm::vec2& position) const;

  float getWaveOffset(const glm::vec2& gridPosition) const;

  void rebuildTileElevationCache();
  void rebuildTerrainElevationGridView(const glm::ivec2& centerTile);

  void sortRenderCommands(std::vector<AnyRenderCommand>& commands);

  void batchTerrainTiles(std::vector<AnyRenderCommand>& commands);

  void submitRenderCommand(const AnyRenderCommand& command,
                           IIsometricRenderer& quadRenderer);

  template <typename T>
  void submitConcreteRenderCommand(const T& concrete,
                                   IIsometricRenderer& quadRenderer);

protected:
  // Dependencies handed to each module's init() (RenderModuleHost).
  ModuleInit moduleInit() override { return {registry, &assetStore}; }

private:
  AssetStore& assetStore;
  IIsometricRenderer& m_quadRenderer;

  RenderQueue<AnyRenderCommand> m_renderQueue;

  std::unordered_map<glm::ivec2, int, IVec2Hash> tileElevationCache;
  std::vector<int> tileElevationGridData;
  TerrainElevationGridView tileElevationGridView;
  bool tileElevationCacheDirty = true;
  // The camera tile the elevation/heightmap window is centered on; the window
  // recenters when the camera crosses a tile, so the heightmap (sun shadows +
  // point-light occlusion) and actor standing elevation follow a moving camera
  // even when terrain isn't otherwise flagged dirty (e.g. a streaming voxel
  // world that doesn't touch the ECS).
  glm::ivec2 tileElevationGridCenter{0, 0};
  bool tileElevationGridCentered = false;

  // Per-actor eased ground elevation (levels), keyed by entity id. Updated in
  // update(); read only when filling each carried light's ground level (so its
  // occlusion ramps smoothly). The sprite itself uses the discrete elevation.
  std::unordered_map<Entity::EntityId, float> m_actorElevation;

  // Optional authoritative terrain heights. When present the heightmap window
  // is filled from this (complete, no streaming holes) rather than the ECS
  // tiles.
  const ITerrainHeightSource* m_terrainHeightSource = nullptr;

  IsometricRenderContext m_context;

  // Sun/sky ambient (set externally) + the frame's point lights gathered from
  // LightEmitterComponent entities. Both are published into m_context each
  // frame.
  IsometricAmbientLighting m_ambient;
  float m_geomClipElevation = 1.0e9f;     // cutaway clip level (levels)
  glm::vec2 m_geomClipCenter{0.0f, 0.0f}; // localized cutaway centre (world)
  float m_geomClipRadius = 0.0f;          // localized cutaway radius (tiles)
  bool m_geomClipActive = false;          // whether the cutaway is engaged
  bool m_hasAmbient = false;
  std::vector<IsometricPointLightSnapshot> m_pointLights;

  // Mirrors the backend's sun-shadow march style so the value can be read back
  // (e.g. for the debug UI dropdown); set via setSunShadowStyle.
  SunShadowStyle m_sunShadowStyle = SunShadowStyle::Smooth;
};

template <typename T>
void IsometricRenderSystem::submitConcreteRenderCommand(
    const T& concrete,
    IIsometricRenderer& quadRenderer)
{
  // Most paths only read the command, so they use `concrete` directly. Only
  // the branches that resolve a texture onto the quad take a mutable copy, and
  // never of a whole batch's quad vector.

  // Terrain shadows use the special stencil-based pipeline so
  // overlapping shadow quads do not stack darker.
  if constexpr (std::is_same_v<T, TerrainShadowCommand>)
  {
    quadRenderer.submitTerrainShadow(concrete.quad);
  }

  // Terrain shadows grouped per painter depth run the same stencil path, one
  // command's worth of quads at a time.
  else if constexpr (std::is_same_v<T, TerrainShadowBatchCommand>)
  {
    for (const auto& quad : concrete.quad.quads)
      quadRenderer.submitTerrainShadow(quad);
  }

  // Projected sprite shadows batch by texture into one draw per shadow atlas.
  else if constexpr (std::is_same_v<T, SpriteShadowCommand>)
  {
    auto drawable = concrete;
    drawable.quad.texture = resolveTexture(drawable.textureId);

    if (drawable.quad.texture != 0)
      quadRenderer.submitSpriteShadow(drawable.quad);
  }

  // Surface meshes (water, lava, fog, etc.)
  else if constexpr (std::is_same_v<T, SurfaceCommand>)
  {
    quadRenderer.submit(concrete);
  }

  // Opt-in real-geometry terrain: a lit, textured face mesh. The renderer can't
  // resolve string texture ids, so resolve here, then hand it the projected,
  // depth-stamped triangle list (assignClipDepth already remapped each vertex
  // z).
  else if constexpr (std::is_same_v<T, GeometryCommand>)
  {
    const unsigned int texture = resolveTexture(concrete.textureId);

    if (texture != 0 && !concrete.vertices.empty())
      quadRenderer.drawGeometry(concrete.vertices.data(),
                                concrete.vertices.size(),
                                texture,
                                static_cast<int>(concrete.type));
  }

  // Baked decals: stamp this frame's new splats into per-target paint textures,
  // refresh any target whose draw set grew, then draw every visible target plus
  // this frame's animating (fading/running) decals. The frame's projection +
  // depth-range uniforms were set in flushBatches.
  else if constexpr (std::is_same_v<T, DecalDrawCommand>)
  {
    for (const auto key : concrete.freeKeys)
      quadRenderer.freePaintTarget(key);

    const unsigned int texture = resolveTexture(concrete.textureId);

    if (texture != 0)
      for (const auto& bake : concrete.bakes)
        quadRenderer.bakeDecals(bake.key,
                                bake.texW,
                                bake.texH,
                                texture,
                                bake.verts.data(),
                                bake.verts.size());

    for (const auto& upload : concrete.drawUploads)
      quadRenderer.uploadPaintDraw(
          upload.key, upload.verts.data(), upload.verts.size());

    for (const auto key : concrete.drawKeys)
      quadRenderer.drawPaintTarget(key);

    if (texture != 0 && !concrete.dynamic.empty())
      quadRenderer.drawDecalsDynamic(
          concrete.dynamic.data(), concrete.dynamic.size(), texture);
  }

  // Batched lit terrain tiles.
  //
  // These are grouped earlier by texture + lighting state to reduce
  // OpenGL state changes and draw calls.
  else if constexpr (std::is_same_v<std::decay_t<decltype(concrete.quad)>,
                                    LitQuadBatch>)
  {
    // Every quad in the batch shares the same material, so resolve the textures
    // and effect once, then submit the whole batch in one call (the renderer
    // sets the batch key once instead of rebuilding it per quad).
    const unsigned int batchTexture = resolveTexture(concrete.textureId);

    if (batchTexture == 0)
      return;

    unsigned int batchNormalTexture = 0;
    bool batchHasNormalMap = false;

    if (concrete.normalTextureId)
    {
      batchNormalTexture = resolveTexture(concrete.normalTextureId);
      batchHasNormalMap = batchNormalTexture != 0;
    }

    int batchSurfaceEffect = 0;

    if constexpr (requires { concrete.type; })
      batchSurfaceEffect = static_cast<int>(concrete.type);

    quadRenderer.submitLitBatch(concrete.quad,
                                batchTexture,
                                batchNormalTexture,
                                batchHasNormalMap,
                                batchSurfaceEffect);
  }

  // Generic solid quad batches.
  //
  // Used for grouped non-lit quads.
  else if constexpr (std::is_same_v<std::decay_t<decltype(concrete.quad)>,
                                    QuadBatch>)
  {
    for (const auto& quad : concrete.quad.quads)
      quadRenderer.submit(quad);
  }

  // Particle billboards: one unlit batch per (texture, blend). World particles
  // (Particles pass) depth-test against the scene; screen-space ones (UI pass)
  // draw on top without depth.
  else if constexpr (std::is_same_v<std::decay_t<decltype(concrete.quad)>,
                                    ParticleBatch>)
  {
    const unsigned int batchTexture = resolveTexture(concrete.textureId);

    if (batchTexture != 0)
    {
      const bool depthTested = concrete.order.pass != RenderPass::UI;
      quadRenderer.submitParticleBatch(
          concrete.quad, batchTexture, concrete.blend, depthTested);
    }
  }

  // Everything else:
  // sprites, UI, standalone lit quads, etc.
  else
  {
    auto drawable = concrete;

    // Resolve main texture if the drawable owns one.
    if constexpr (requires {
                    drawable.textureId;
                    drawable.quad.texture;
                  })
    {
      drawable.quad.texture = resolveTexture(drawable.textureId);

      if (drawable.quad.texture == 0)
        return;
    }

    // Resolve optional normal map.
    if constexpr (requires {
                    drawable.normalTextureId;
                    drawable.quad.normalTexture;
                  })
    {
      drawable.quad.normalTexture = resolveTexture(drawable.normalTextureId);

      drawable.quad.hasNormalMap = drawable.quad.normalTexture != 0;
    }

    if constexpr (requires {
                    drawable.effect;
                    drawable.quad.surfaceEffect;
                  })
    {
      drawable.quad.surfaceEffect = static_cast<int>(drawable.effect);
    }

    // Standard submission path.
    quadRenderer.submit(drawable.quad);
  }
}
} // namespace sfs
