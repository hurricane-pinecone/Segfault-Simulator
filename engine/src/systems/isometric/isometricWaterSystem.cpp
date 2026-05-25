#include "engine/systems/isometric/isometricWaterSystem.h"
#include "engine/Color/Color.h"
#include "engine/components/elevationComponent.h"
#include "engine/components/waterTileComponent.h"
#include "engine/ecs/ecs.h" // IWYU pragma: keep
#include "engine/rendering/isometricRenderContext.h"

namespace sfs
{

void IsometricWaterSystem::create()
{
  registerComponent<WaterTileComponent>();
  registerComponent<ElevationComponent>();
  registerComponent<TransformComponent>();
}

void IsometricWaterSystem::computeCommands(
    const IsometricRenderContext& context)
{
  flush();

  for (const auto& entity : getEntities())
  {
    constructRenderCommands(context, entity);
  }
}

void IsometricWaterSystem::constructRenderCommands(
    const IsometricRenderContext& context,
    const Entity& entity)
{
  const glm::vec2 isoCameraPosition = context.activeCamera.isoPosition(
      context.tileWidth, context.tileHeight, context.worldScale);
  const auto zoom =
      context.activeCamera.camera ? context.activeCamera.camera->zoom : 1;

  const auto& transform = entity.getComponent<TransformComponent>();
  const auto& water = entity.getComponent<WaterTileComponent>();

  auto project = [&](const glm::vec2& world)
  {
    glm::vec2 p =
        (gridToIsometric(
             world, context.tileWidth, context.tileHeight, context.worldScale) -
         isoCameraPosition) *
            zoom +
        context.screenCenter;

    p.y -= static_cast<float>(water.elevation) * context.elevationStep *
           context.worldScale * zoom;

    return p;
  };

  const glm::ivec2 tile = gridCellOf(transform.position);

  float depthSum = 0.0f;
  float weightSum = 0.0f;

  for (int oy = -1; oy <= 1; oy++)
  {
    for (int ox = -1; ox <= 1; ox++)
    {
      const glm::ivec2 sampleTile = tile + glm::ivec2{ox, oy};

      int sampleElevation = 0;

      if (!context.terrainElevationGrid.tryGet(sampleTile, sampleElevation))
        continue;

      const float sampleDepth =
          static_cast<float>(std::max(0, water.elevation - sampleElevation));

      const float weight = ox == 0 && oy == 0 ? 2.0f : 1.0f;

      depthSum += sampleDepth * weight;
      weightSum += weight;
    }
  }

  const float smoothedDepth = weightSum > 0.0f ? depthSum / weightSum : 0.0f;

  float depthFactor = std::clamp(smoothedDepth / 6.0f, 0.0f, 1.0f);

  SDL_Color shallow{55, 155, 210, 70};
  SDL_Color deep = sfs::Colors::Navy.toSDL();

  auto lerpByte = [](Uint8 a, Uint8 b, float t)
  {
    return static_cast<Uint8>(
        std::round(static_cast<float>(a) +
                   (static_cast<float>(b) - static_cast<float>(a)) * t));
  };

  QuadCommand command;

  command.quad.points[0] = project(transform.position + glm::vec2{0, 0});
  command.quad.points[1] = project(transform.position + glm::vec2{1, 0});
  command.quad.points[2] = project(transform.position + glm::vec2{1, 1});
  command.quad.points[3] = project(transform.position + glm::vec2{0, 1});
  const Uint8 baseAlpha = lerpByte(shallow.a, deep.a, depthFactor);

  command.quad.tint = SDL_Color{
      lerpByte(shallow.r, deep.r, depthFactor),
      lerpByte(shallow.g, deep.g, depthFactor),
      lerpByte(shallow.b, deep.b, depthFactor),
      static_cast<Uint8>(std::round(static_cast<float>(baseAlpha))),
  };

  const float sort = transform.position.x + transform.position.y +
                     static_cast<float>(water.elevation) * 0.5f;

  command.order = RenderOrder{RenderPass::Terrain, sort, 3};

  m_commands.push_back(command);
}

} // namespace sfs
