#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/components/transformComponent.h"
#include "engine/ecs/ecs.h" // IWYU pragma: keep
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/iQuadRenderer.h"
#include "engine/rendering/iTerrainHeightSource.h"
#include "engine/rendering/isometricRenderContext.h"
#include "engine/rendering/renderQueue.h"
#include "engine/rendering/util/isometric/camera.h"
#include "engine/systems/isometric/isometricLightingSystem.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_int2.hpp"

#include <SDL_pixels.h>
#include <SDL_rect.h>

namespace sfs
{

extern int gTerrainShadowItems;
extern int gTerrainShadowFlushes;
extern int gRenderItemCount;
extern int gTerrainShadowEdgesProcessed;
extern int gTileRenderItems;
extern int gSpriteRenderItems;
extern int gSpriteProjectedShadowItems;
extern int gTerrainShadowBatchCount;

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

class IsometricRenderSystem : public System
{
public:
  IsometricRenderSystem(AssetStore& assetStore, IQuadRenderer& quadRenderer);

  ~IsometricRenderSystem();

  void setProjection(const IsometricProjection* projection);

  // Authoritative terrain heights for the point-light occlusion heightmap. When
  // set, the heightmap samples this instead of the per-tile ECS entities, so its
  // window is hole-free even while terrain streams in (see ITerrainHeightSource).
  void setTerrainHeightSource(const ITerrainHeightSource* source);

  void setWaveTime(float time);
  void setWaveEnabled(bool enabled);

  void drawDebugTile(const glm::vec2& gridPosition,
                     SDL_Color color = SDL_Color{255, 255, 0, 255});

  void drawDebugTile(const glm::vec2& gridPosition,
                     int elevation,
                     SDL_Color color = SDL_Color{255, 255, 0, 255});

  void markTerrainDirty();

  IsometricLightingService& lighting();
  const IsometricLightingService& lighting() const;

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

  // Eases each actor's rendered ground elevation toward the tile it stands on, so
  // crossing an elevation step ramps smoothly instead of teleporting a whole
  // level (which jumps both the sprite and any light it carries).
  void updateActorElevations(double deltaTime);

  // Smoothed ground elevation (levels) for the actor, or its instantaneous tile
  // elevation if it isn't being tracked yet.
  float smoothedElevationOf(const Entity& entity,
                            const glm::vec2& samplePosition) const;

  void flushBatches();

  unsigned int resolveTexture(const std::string* textureId);

  bool isTileEntity(const Entity& entity) const;

  float getRenderElevationLevel(const Entity& entity,
                                const glm::vec2& samplePosition) const;

  glm::vec2 getGroundSamplePosition(const Entity& entity,
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
                           IQuadRenderer& quadRenderer);

  template <typename T>
  void submitConcreteRenderCommand(const T& concrete,
                                   IQuadRenderer& quadRenderer);

private:
  AssetStore& assetStore;
  IQuadRenderer& m_quadRenderer;

  RenderQueue<AnyRenderCommand> m_renderQueue;

  std::unordered_map<glm::ivec2, int, IVec2Hash> tileElevationCache;
  std::vector<int> tileElevationGridData;
  TerrainElevationGridView tileElevationGridView;
  bool tileElevationCacheDirty = true;

  // Per-actor eased ground elevation (levels), keyed by entity id. Updated in
  // update(); read when placing sprites and when filling each light's ground.
  std::unordered_map<Entity::EntityId, float> m_actorElevation;

  // Optional authoritative terrain heights. When present the heightmap window is
  // filled from this (complete, no streaming holes) rather than the ECS tiles.
  const ITerrainHeightSource* m_terrainHeightSource = nullptr;

  IsometricRenderContext m_context;
  IsometricLightingService m_lightingService;
};

template <typename T>
void IsometricRenderSystem::submitConcreteRenderCommand(
    const T& concrete,
    IQuadRenderer& quadRenderer)
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
