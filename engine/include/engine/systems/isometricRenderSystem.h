#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/components/transformComponent.h"
#include "engine/ecs/system.h"

#include "engine/systems/isometricLightingSystem.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_int2.hpp"

#include <SDL_pixels.h>
#include <SDL_rect.h>

#include <string>
#include <vector>

namespace sfs
{

struct IsometricTile
{
};

struct ElevationComponent
{
  int level = 0;

  ElevationComponent(int level = 0);
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

  void setWorldScale(float scale);
  float getWorldScale() const;

  IsometricRenderSystem(const IsometricRenderSystem&) = delete;
  IsometricRenderSystem& operator=(const IsometricRenderSystem&) = delete;

private:
  struct RenderItem
  {
    std::string textureId;

    SDL_Rect srcRect{0, 0, 0, 0};
    SDL_Rect dest{0, 0, 0, 0};

    int textureWidth = 0;
    int textureHeight = 0;

    float sortKey = 0.0f;
    int renderLayer = 1;

    bool hasNormalMap = false;
    std::string normalTextureId;
    SDL_Rect normalSrcRect{0, 0, 0, 0};
    int normalTextureWidth = 0;
    int normalTextureHeight = 0;

    IsometricComputedLighting lighting{};

    bool isShadow = false;
    glm::vec2 shadowOffset{0.0f, 0.0f};

    SDL_Color tint{255, 255, 255, 255};
  };

private:
  void beginBatches();
  void submitSprite(const RenderItem& item);
  void submitShadow(const RenderItem& caster,
                    const glm::vec2& shadowOffset,
                    float alpha,
                    float sortKeyBias = -0.004f);

  void flushBatches();

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

private:
  AssetStore& assetStore;

  int windowWidth = 0;
  int windowHeight = 0;

  float worldScale = 1.0f;

  int tileWidth = 0;
  int tileHeight = 0;

  int elevationStep = 8;

  bool waveEnabled = true;
  float waveTime = 0.0f;
  float waveAmplitude = 6.0f;
  float waveFrequency = 0.45f;
  float waveSpeed = 3.0f;

  std::vector<RenderItem> renderItems;
};

} // namespace sfs
