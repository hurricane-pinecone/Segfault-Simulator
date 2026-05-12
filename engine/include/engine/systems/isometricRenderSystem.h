#pragma once

#include "engine/systems/cameraSystem.h"

#include <SDL_rect.h>
#include <SDL_render.h>

#include <algorithm>
#include <cmath>

#include <engine/assetStore/assetStore.h>
#include <engine/components/spriteComponent.h>
#include <engine/components/transformComponent.h>
#include <engine/ecs/system.h>
#include <engine/logger/logger.h>

#include <glm/glm/ext/vector_float2.hpp>
#include <glm/glm/ext/vector_int2.hpp>

namespace sfs
{

struct IsometricTile
{
};

struct ElevationComponent
{
  int level = 0;

  ElevationComponent(int level = 0) : level(level) {}
};

class IsometricRenderSystem : public System
{
public:
  IsometricRenderSystem(AssetStore& assetStore,
                        int windowWidth,
                        int windowHeight,
                        int tileWidth,
                        int tileHeight)
      : assetStore(assetStore), windowWidth(windowWidth),
        windowHeight(windowHeight), tileWidth(tileWidth), tileHeight(tileHeight)
  {
    registerComponent<SpriteComponent>();
    registerComponent<TransformComponent>();
  }

  void render(SDL_Renderer& renderer)
  {
    const glm::vec2 cameraPosition = getCameraPosition();

    const glm::vec2 screenCenter{static_cast<float>(windowWidth) / 2.0f,
                                 static_cast<float>(windowHeight) / 2.0f};

    const glm::vec2 isoCameraPosition = gridToIsometric(cameraPosition);

    for (const auto& entity : getEntities())
    {
      const auto& transform = entity.getComponent<TransformComponent>();

      const auto& spriteComponent = entity.getComponent<SpriteComponent>();

      const auto sprite = assetStore.getSprite(spriteComponent.spriteId);

      if (!sprite)
      {
        LOG_ERROR("Attempted to render NULL isometric sprite");
        continue;
      }

      SDL_Texture* texture = assetStore.getTexture(sprite->textureId);

      if (!texture)
      {
        LOG_ERROR("NULL texture included in isometric render loop");
        continue;
      }

      const glm::vec2 isoPosition = gridToIsometric(transform.position);

      const glm::vec2 screenPosition =
          isoPosition - isoCameraPosition + screenCenter;

      const int width = static_cast<int>(sprite->srcRect.w * transform.scale.x);

      const int height =
          static_cast<int>(sprite->srcRect.h * transform.scale.y);

      const float anchorX =
          spriteComponent.anchor.x * static_cast<float>(width);

      const float anchorY =
          spriteComponent.anchor.y * static_cast<float>(height);

      const glm::vec2 groundSamplePosition =
          getGroundSamplePosition(entity, transform);

      const int elevationLevel =
          getRenderElevationLevel(entity, groundSamplePosition);

      const int elevationOffset = elevationLevel * elevationStep;

      const float waveOffset = getWaveOffset(groundSamplePosition);

      const glm::vec2 surfacePosition{
          screenPosition.x, screenPosition.y - elevationOffset - waveOffset};

      SDL_Rect dest{static_cast<int>(std::round(surfacePosition.x - anchorX)),
                    static_cast<int>(std::round(surfacePosition.y - anchorY)),
                    width,
                    height};

      SDL_RenderCopyEx(&renderer,
                       texture,
                       &sprite->srcRect,
                       &dest,
                       0.0,
                       nullptr,
                       SDL_FLIP_NONE);
    }
  }

  void setWaveTime(float time) { waveTime = time; }

  void setWaveEnabled(bool enabled) { waveEnabled = enabled; }

  void drawDebugTile(SDL_Renderer& renderer,
                     const glm::vec2& gridPosition,
                     SDL_Color color = SDL_Color{255, 255, 0, 255})
  {
    drawDebugTile(
        renderer, gridPosition, getTileElevationAt(gridPosition), color);
  }

  void drawDebugTile(SDL_Renderer& renderer,
                     const glm::vec2& gridPosition,
                     int elevation,
                     SDL_Color color = SDL_Color{255, 255, 0, 255})
  {
    const glm::vec2 cameraPosition = getCameraPosition();

    const glm::vec2 screenCenter{static_cast<float>(windowWidth) / 2.0f,
                                 static_cast<float>(windowHeight) / 2.0f};

    glm::vec2 screenPosition = gridToIsometric(gridPosition) -
                               gridToIsometric(cameraPosition) + screenCenter;

    screenPosition.y -= elevation * elevationStep;
    screenPosition.y -= getWaveOffset(gridPosition);

    SDL_Point points[5] = {
        {static_cast<int>(std::round(screenPosition.x)),
         static_cast<int>(std::round(screenPosition.y))},

        {static_cast<int>(std::round(screenPosition.x + tileWidth / 2.0f)),
         static_cast<int>(std::round(screenPosition.y + tileHeight / 2.0f))},

        {static_cast<int>(std::round(screenPosition.x)),
         static_cast<int>(std::round(screenPosition.y + tileHeight))},

        {static_cast<int>(std::round(screenPosition.x - tileWidth / 2.0f)),
         static_cast<int>(std::round(screenPosition.y + tileHeight / 2.0f))},

        {static_cast<int>(std::round(screenPosition.x)),
         static_cast<int>(std::round(screenPosition.y))}};

    SDL_SetRenderDrawColor(&renderer, color.r, color.g, color.b, color.a);
    SDL_RenderDrawLines(&renderer, points, 5);
  }

  IsometricRenderSystem(const IsometricRenderSystem&) = delete;
  IsometricRenderSystem& operator=(const IsometricRenderSystem&) = delete;

private:
  glm::vec2 getCameraPosition() const
  {
    if (!registry->hasSystem<CameraSystem>())
      return {0.0f, 0.0f};

    const auto& camera = registry->getSystem<CameraSystem>().getEntities();

    if (camera.empty())
      return {0.0f, 0.0f};

    return camera[0].getComponent<TransformComponent>().position;
  }

  glm::vec2 gridToIsometric(const glm::vec2& gridPosition) const
  {
    return {(gridPosition.x - gridPosition.y) * static_cast<float>(tileWidth) /
                2.0f,

            (gridPosition.x + gridPosition.y) * static_cast<float>(tileHeight) /
                2.0f};
  }

  glm::ivec2 gridCellOf(const glm::vec2& position) const
  {
    return {static_cast<int>(std::floor(position.x)),
            static_cast<int>(std::floor(position.y))};
  }

  bool isTileEntity(const Entity& entity) const
  {
    return entity.hasComponent<IsometricTile>();
  }

  glm::vec2
  getElevationSamplePosition(const Entity& entity,
                             const TransformComponent& transform,
                             const SpriteComponent& spriteComponent) const
  {
    return transform.position;
  }

  int getRenderElevationLevel(const Entity& entity,
                              const glm::vec2& samplePosition) const
  {
    if (isTileEntity(entity))
    {
      if (entity.hasComponent<ElevationComponent>())
        return entity.getComponent<ElevationComponent>().level;

      return 0;
    }

    return getTileElevationAt(samplePosition);
  }

  glm::vec2 getGroundSamplePosition(const Entity& entity,
                                    const TransformComponent& transform) const
  {
    if (isTileEntity(entity))
      return transform.position;

    return glm::floor(transform.position);
  }

  bool tryGetTileElevationAt(const glm::vec2& position, int& outElevation) const
  {
    const glm::ivec2 targetTile = gridCellOf(position);

    bool found = false;
    int highest = 0;

    for (const auto& entity : getEntities())
    {
      if (!isTileEntity(entity))
        continue;

      if (!entity.hasComponent<ElevationComponent>())
        continue;

      const auto& transform = entity.getComponent<TransformComponent>();

      if (gridCellOf(transform.position) != targetTile)
        continue;

      const auto& elevation = entity.getComponent<ElevationComponent>();

      highest = std::max(highest, elevation.level);
      found = true;
    }

    outElevation = highest;
    return found;
  }

  int getTileElevationAt(const glm::vec2& position) const
  {
    int elevation = 0;
    tryGetTileElevationAt(position, elevation);
    return elevation;
  }

  float getWaveOffset(const glm::vec2& gridPosition) const
  {
    if (!waveEnabled)
      return 0.0f;

    return std::sin(gridPosition.x * waveFrequency +
                    gridPosition.y * waveFrequency + waveTime * waveSpeed) *
           waveAmplitude;
  }

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
