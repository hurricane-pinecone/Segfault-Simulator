#include "engine/systems/isometricRenderSystem.h"

#include "engine/ecs/registry.h"
#include "engine/logger/logger.h"
#include "engine/systems/cameraSystem.h"
#include "engine/systems/isometricLightingSystem.h"

#include <algorithm>
#include <cmath>

namespace sfs
{

ElevationComponent::ElevationComponent(int level) : level(level) {}

IsometricRenderSystem::IsometricRenderSystem(AssetStore& assetStore,
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

void IsometricRenderSystem::render(SDL_Renderer& renderer)
{
  auto* lightingSystem = registry->hasSystem<IsometricLightingSystem>()
                             ? &registry->getSystem<IsometricLightingSystem>()
                             : nullptr;

  if (lightingSystem)
    lightingSystem->rebuildLights();

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

    const int height = static_cast<int>(sprite->srcRect.h * transform.scale.y);

    const float anchorX = spriteComponent.anchor.x * static_cast<float>(width);

    const float anchorY = spriteComponent.anchor.y * static_cast<float>(height);

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

    glm::vec2 spriteWorldSample =
        isTileEntity(entity) ? transform.position + glm::vec2{0.5f, 0.5f}
                             : transform.position;

    if (lightingSystem)
    {
      IsometricLightingSample lightingSample{
          spriteWorldSample, static_cast<float>(elevationOffset)};

      if (lightingSystem->renderLitSprite(
              renderer, entity, spriteComponent, *sprite, dest, lightingSample))
      {
        continue;
      }
    }

    SDL_SetTextureColorMod(texture, 255, 255, 255);

    SDL_RenderCopyEx(&renderer,
                     texture,
                     &sprite->srcRect,
                     &dest,
                     0.0,
                     nullptr,
                     SDL_FLIP_NONE);

    SDL_SetTextureColorMod(texture, 255, 255, 255);
  }
}

void IsometricRenderSystem::setWaveTime(float time) { waveTime = time; }

void IsometricRenderSystem::setWaveEnabled(bool enabled)
{
  waveEnabled = enabled;
}

void IsometricRenderSystem::drawDebugTile(SDL_Renderer& renderer,
                                          const glm::vec2& gridPosition,
                                          SDL_Color color)
{
  drawDebugTile(
      renderer, gridPosition, getTileElevationAt(gridPosition), color);
}

void IsometricRenderSystem::drawDebugTile(SDL_Renderer& renderer,
                                          const glm::vec2& gridPosition,
                                          int elevation,
                                          SDL_Color color)
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

void IsometricRenderSystem::setLightDirection(const glm::vec3& direction)
{
  if (!registry->hasSystem<IsometricLightingSystem>())
    return;

  registry->getSystem<IsometricLightingSystem>().setLightDirection(direction);
}

void IsometricRenderSystem::setLighting(float ambient, float diffuseStrength)
{
  if (!registry->hasSystem<IsometricLightingSystem>())
    return;

  registry->getSystem<IsometricLightingSystem>().setLighting(
      ambient, diffuseStrength);
}

glm::vec2
IsometricRenderSystem::gridToIsometric(const glm::vec2& gridPosition) const
{
  return {
      (gridPosition.x - gridPosition.y) * static_cast<float>(tileWidth) / 2.0f,

      (gridPosition.x + gridPosition.y) * static_cast<float>(tileHeight) /
          2.0f};
}

glm::vec2 IsometricRenderSystem::isometricToGrid(const glm::vec2& iso) const
{
  float x = (iso.x / (tileWidth / 2.0f) + iso.y / (tileHeight / 2.0f)) * 0.5f;

  float y = (iso.y / (tileHeight / 2.0f) - iso.x / (tileWidth / 2.0f)) * 0.5f;

  return {x, y};
}

glm::vec2 IsometricRenderSystem::getCameraPosition() const
{
  if (!registry->hasSystem<CameraSystem>())
    return {0.0f, 0.0f};

  const auto& camera = registry->getSystem<CameraSystem>().getEntities();

  if (camera.empty())
    return {0.0f, 0.0f};

  return camera[0].getComponent<TransformComponent>().position;
}

glm::ivec2 IsometricRenderSystem::gridCellOf(const glm::vec2& position) const
{
  return {static_cast<int>(std::floor(position.x)),
          static_cast<int>(std::floor(position.y))};
}

bool IsometricRenderSystem::isTileEntity(const Entity& entity) const
{
  return entity.hasComponent<IsometricTile>();
}

int IsometricRenderSystem::getRenderElevationLevel(
    const Entity& entity,
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

glm::vec2 IsometricRenderSystem::getGroundSamplePosition(
    const Entity& entity,
    const TransformComponent& transform) const
{
  if (isTileEntity(entity))
    return transform.position;

  return glm::floor(transform.position);
}

bool IsometricRenderSystem::tryGetTileElevationAt(const glm::vec2& position,
                                                  int& outElevation) const
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

int IsometricRenderSystem::getTileElevationAt(const glm::vec2& position) const
{
  int elevation = 0;
  tryGetTileElevationAt(position, elevation);
  return elevation;
}

float IsometricRenderSystem::getWaveOffset(const glm::vec2& gridPosition) const
{
  if (!waveEnabled)
    return 0.0f;

  return std::sin(gridPosition.x * waveFrequency +
                  gridPosition.y * waveFrequency + waveTime * waveSpeed) *
         waveAmplitude;
}

} // namespace sfs
