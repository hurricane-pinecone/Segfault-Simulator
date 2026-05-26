#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/components/transformComponent.h"
#include "engine/ecs/ecs.h" // IWYU pragma: keep
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/isometricRenderContext.h"
#include "engine/rendering/openGLQuadRenderer.h"
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
  IsometricRenderSystem(AssetStore& assetStore,
                        int windowWidth,
                        int windowHeight,
                        int tileWidth,
                        int tileHeight,
                        int elevationStep,
                        float worldScale);

  ~IsometricRenderSystem();

  void setWaveTime(float time);
  void setWaveEnabled(bool enabled);

  void drawDebugTile(const glm::vec2& gridPosition,
                     SDL_Color color = SDL_Color{255, 255, 0, 255});

  void drawDebugTile(const glm::vec2& gridPosition,
                     int elevation,
                     SDL_Color color = SDL_Color{255, 255, 0, 255});

  void markTerrainDirty();

  void setWorldScale(float scale);
  float getWorldScale() const;

  IsometricLightingService& lighting();
  const IsometricLightingService& lighting() const;

  IsometricRenderSystem(const IsometricRenderSystem&) = delete;
  IsometricRenderSystem& operator=(const IsometricRenderSystem&) = delete;

protected:
  void create() override;
  void render() override;

private:
  void beginBatches();

  void flushBatches();

  unsigned int resolveTexture(const std::string* textureId);

  glm::vec2 getCameraPosition() const;

  bool isTileEntity(const Entity& entity) const;

  int getRenderElevationLevel(const Entity& entity,
                              const glm::vec2& samplePosition) const;

  glm::vec2 getGroundSamplePosition(const Entity& entity,
                                    const TransformComponent& transform) const;

  bool tryGetTileElevationAt(const glm::vec2& position,
                             int& outElevation) const;

  int getTileElevationAt(const glm::vec2& position) const;

  float getWaveOffset(const glm::vec2& gridPosition) const;

  ActiveCamera getCamera() const;

  void rebuildTileElevationCache();
  void rebuildTerrainElevationGridView();

  void sortRenderCommands(std::vector<AnyRenderCommand>& commands);

  void batchTerrainTiles(std::vector<AnyRenderCommand>& commands);

  void submitRenderCommand(const AnyRenderCommand& command,
                           OpenGLQuadRenderer& quadRenderer);

  template <typename T>
  void submitConcreteRenderCommand(const T& concrete,
                                   OpenGLQuadRenderer& quadRenderer);

private:
  AssetStore& assetStore;

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
    OpenGLQuadRenderer& quadRenderer)
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
    for (auto& quad : drawable.quad.quads)
    {
      // Resolve diffuse/albedo texture.
      quad.texture = resolveTexture(drawable.textureId);

      if (quad.texture == 0)
        continue;

      // Optional normal map for lighting.
      if (drawable.normalTextureId)
      {
        quad.normalTexture = resolveTexture(drawable.normalTextureId);

        quad.hasNormalMap = quad.normalTexture != 0;
      }

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

    // Standard submission path.
    quadRenderer.submit(drawable.quad);
  }
}
} // namespace sfs
