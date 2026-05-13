#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/components/transformComponent.h"
#include "engine/ecs/system.h"

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_int2.hpp"

#include <SDL_rect.h>
#include <SDL_render.h>

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

  void render(SDL_Renderer& renderer);

  void setWaveTime(float time);
  void setWaveEnabled(bool enabled);

  void drawDebugTile(SDL_Renderer& renderer,
                     const glm::vec2& gridPosition,
                     SDL_Color color = SDL_Color{255, 255, 0, 255});

  void drawDebugTile(SDL_Renderer& renderer,
                     const glm::vec2& gridPosition,
                     int elevation,
                     SDL_Color color = SDL_Color{255, 255, 0, 255});

  void setLightDirection(const glm::vec3& direction);
  void setLighting(float ambient, float diffuseStrength);

  glm::vec2 gridToIsometric(const glm::vec2& gridPosition) const;
  glm::vec2 isometricToGrid(const glm::vec2& iso) const;

  IsometricRenderSystem(const IsometricRenderSystem&) = delete;
  IsometricRenderSystem& operator=(const IsometricRenderSystem&) = delete;

private:
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

  int windowWidth;
  int windowHeight;

  int tileWidth;
  int tileHeight;

  int elevationStep = 8;

  bool waveEnabled = true;
  float waveTime = 0.0f;
  float waveAmplitude = 6.0f;
  float waveFrequency = 0.45f;
  float waveSpeed = 3.0f;
};

} // namespace sfs
