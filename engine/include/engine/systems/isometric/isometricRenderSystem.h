#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/components/cameraComponent.h"
#include "engine/components/transformComponent.h"
#include "engine/ecs/system.h"

#include "engine/renderers/commands/commands.h"
#include "engine/renderers/isometricRenderContext.h"
#include "engine/renderers/renderQueue.h"
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

struct ActiveCamera
{
  const CameraComponent* camera = nullptr;
  const TransformComponent* transform = nullptr;
};

class IsometricRenderSystem : public System
{
public:
  IsometricRenderSystem(AssetStore& assetStore,
                        int windowWidth,
                        int windowHeight,
                        int tileWidth,
                        int tileHeight);

  ~IsometricRenderSystem();

  void render();

  void setWaveTime(float time);
  void setWaveEnabled(bool enabled);

  void drawDebugTile(const glm::vec2& gridPosition,
                     SDL_Color color = SDL_Color{255, 255, 0, 255});

  void drawDebugTile(const glm::vec2& gridPosition,
                     int elevation,
                     SDL_Color color = SDL_Color{255, 255, 0, 255});

  glm::vec2 gridToIsometric(const glm::vec2& gridPosition) const;
  glm::vec2 isometricToGrid(const glm::vec2& iso) const;

  void markTerrainDirty();

  void setWorldScale(float scale);
  float getWorldScale() const;

  IsometricRenderSystem(const IsometricRenderSystem&) = delete;
  IsometricRenderSystem& operator=(const IsometricRenderSystem&) = delete;

private:
  void beginBatches();

  void flushBatches();

  unsigned int resolveTexture(const std::string* textureId);

  glm::vec2 getCameraPosition() const;
  glm::ivec2 gridCellOf(const glm::vec2& position) const;

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

private:
  AssetStore& assetStore;

  int windowWidth = 0;
  int windowHeight = 0;

  float worldScale = 1.0f;

  int tileWidth = 0;
  int tileHeight = 0;

  int elevationStep = 8;

  bool waveEnabled = false;
  float waveTime = 0.0f;
  float waveAmplitude = 6.0f;
  float waveFrequency = 0.45f;
  float waveSpeed = 3.0f;

  RenderQueue<AnyRenderCommand> m_renderQueue;

  std::unordered_map<glm::ivec2, int, IVec2Hash> tileElevationCache;
  std::vector<int> tileElevationGridData;
  TerrainElevationGridView tileElevationGridView;
  bool tileElevationCacheDirty = true;

  IsometricRenderContext m_context;
};

} // namespace sfs
