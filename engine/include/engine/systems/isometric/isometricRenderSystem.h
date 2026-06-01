#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/components/transformComponent.h"
#include "engine/ecs/ecs.h" // IWYU pragma: keep
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/iQuadRenderer.h"
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
  void render() override;

private:
  void beginBatches();

  void flushBatches();

  unsigned int resolveTexture(const std::string* textureId);

  bool isTileEntity(const Entity& entity) const;

  int getRenderElevationLevel(const Entity& entity,
                              const glm::vec2& samplePosition) const;

  glm::vec2 getGroundSamplePosition(const Entity& entity,
                                    const TransformComponent& transform) const;

  bool tryGetTileElevationAt(const glm::vec2& position,
                             int& outElevation) const;

  int getTileElevationAt(const glm::vec2& position) const;

  float getWaveOffset(const glm::vec2& gridPosition) const;

  void rebuildTileElevationCache();
  void rebuildTerrainElevationGridView();

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

  IsometricRenderContext m_context;
  IsometricLightingService m_lightingService;
};

template <typename T>
void IsometricRenderSystem::submitConcreteRenderCommand(
    const T& concrete,
    IQuadRenderer& quadRenderer)
{
  // Work on a mutable local copy because textures and normal maps
  // are resolved lazily during submission.
  auto drawable = concrete;

  // Terrain shadows use the special stencil-based pipeline so
  // overlapping shadow quads do not stack darker.
  if constexpr (std::is_same_v<T, TerrainShadowCommand>)
  {
    quadRenderer.submitTerrainShadow(drawable.quad);
  }

  // Terrain shadows grouped per painter depth run the same stencil path, one
  // command's worth of quads at a time.
  else if constexpr (std::is_same_v<T, TerrainShadowBatchCommand>)
  {
    for (const auto& quad : drawable.quad.quads)
      quadRenderer.submitTerrainShadow(quad);
  }

  // Projected sprite shadows batch by texture into one draw per shadow atlas.
  else if constexpr (std::is_same_v<T, SpriteShadowCommand>)
  {
    drawable.quad.texture = resolveTexture(drawable.textureId);

    if (drawable.quad.texture != 0)
      quadRenderer.submitSpriteShadow(drawable.quad);
  }

  // Surface meshes (water, lava, fog, etc.)
  else if constexpr (std::is_same_v<T, SurfaceCommand>)
  {
    quadRenderer.submit(drawable);
  }

  // Batched lit terrain tiles.
  //
  // These are grouped earlier by texture + lighting state to reduce
  // OpenGL state changes and draw calls.
  else if constexpr (std::is_same_v<std::decay_t<decltype(drawable.quad)>,
                                    LitQuadBatch>)
  {
    // Every quad in the batch shares the same material, so resolve the textures
    // and effect once instead of once per quad.
    const unsigned int batchTexture = resolveTexture(drawable.textureId);

    if (batchTexture == 0)
      return;

    unsigned int batchNormalTexture = 0;
    bool batchHasNormalMap = false;

    if (drawable.normalTextureId)
    {
      batchNormalTexture = resolveTexture(drawable.normalTextureId);
      batchHasNormalMap = batchNormalTexture != 0;
    }

    int batchSurfaceEffect = 0;

    if constexpr (requires { drawable.type; })
      batchSurfaceEffect = static_cast<int>(drawable.type);

    for (auto& quad : drawable.quad.quads)
    {
      quad.texture = batchTexture;
      quad.normalTexture = batchNormalTexture;
      quad.hasNormalMap = batchHasNormalMap;
      quad.surfaceEffect = batchSurfaceEffect;

      quadRenderer.submit(quad);
    }
  }

  // Generic solid quad batches.
  //
  // Used for grouped non-lit quads.
  else if constexpr (std::is_same_v<std::decay_t<decltype(drawable.quad)>,
                                    QuadBatch>)
  {
    for (const auto& quad : drawable.quad.quads)
      quadRenderer.submit(quad);
  }

  // Everything else:
  // sprites, UI, standalone lit quads, etc.
  else
  {
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
