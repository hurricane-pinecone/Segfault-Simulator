#include "engine/systems/isometric/isometricWaterSystem.h"
#include "engine/Color/Color.h"
#include "engine/components/waterTileComponent.h"
#include "engine/ecs/ecs.h" // IWYU pragma: keep
#include "engine/rendering/isometricRenderContext.h"
#include <cstdint>

namespace sfs
{

void IsometricWaterSystem::create()
{
  registerComponent<WaterTileComponent>();
  registerComponent<TransformComponent>();
}

void IsometricWaterSystem::computeCommands(
    const IsometricRenderContext& context)
{
  flush();

  WaterSurfaceBuild build = collectWaterSurfaceBuild(context);

  if (build.cells.empty())
    return;

  SurfaceCommand command = createWaterSurfaceCommand();
  buildWaterSurfaceMesh(context, build, command);

  if (!command.vertices.empty() && !command.indices.empty())
    m_commands.push_back(std::move(command));
}

SurfaceCommand IsometricWaterSystem::createWaterSurfaceCommand() const
{
  SurfaceCommand command;
  command.material = SurfaceCommand::SurfaceMaterial::Water;
  command.order = RenderOrder{RenderPass::Surfaces, 0.0f, 0};
  return command;
}

WaterSurfaceBuild IsometricWaterSystem::collectWaterSurfaceBuild(
    const IsometricRenderContext& context) const
{
  WaterSurfaceBuild build;
  build.cells.reserve(getEntities().size());

  bool hasBounds = false;

  for (const auto& entity : getEntities())
  {
    const auto& transform = entity.getComponent<TransformComponent>();
    const auto& water = entity.getComponent<WaterTileComponent>();

    const glm::ivec2 tile = gridCellOf(transform.position);
    int terrainElevation = 0;
    context.terrainElevationGrid.tryGet(tile, terrainElevation);

    build.cells.push_back(WaterCell{
        tile,
        water.elevation,
        static_cast<float>(std::max(0, water.elevation - terrainElevation))});

    if (!hasBounds)
    {
      build.minTile = tile;
      build.maxTile = tile;
      hasBounds = true;
    }
    else
    {
      build.minTile = glm::min(build.minTile, tile);
      build.maxTile = glm::max(build.maxTile, tile);
    }
  }

  return build;
}

void IsometricWaterSystem::buildWaterSurfaceMesh(
    const IsometricRenderContext& context,
    const WaterSurfaceBuild& build,
    SurfaceCommand& command) const
{
  const int cellWidth = build.maxTile.x - build.minTile.x + 1;
  const int cellHeight = build.maxTile.y - build.minTile.y + 1;

  std::vector<float> cellDepths(
      static_cast<size_t>(cellWidth * cellHeight), -1.0f);

  auto cellIndex = [&](const glm::ivec2& tile)
  {
    const int x = tile.x - build.minTile.x;
    const int y = tile.y - build.minTile.y;
    return static_cast<size_t>(y * cellWidth + x);
  };

  command.vertices.reserve(static_cast<size_t>(build.cells.size() * 4));

  for (const WaterCell& cell : build.cells)
  {
    cellDepths[cellIndex(cell.tile)] = cell.depth;
  }

  for (const WaterCell& cell : build.cells)
  {
    const glm::ivec2 tile = cell.tile;

    const float d0 = sampleSurfaceVertexDepth(tile + glm::ivec2{0, 0},
                                              build.minTile,
                                              cellWidth,
                                              cellHeight,
                                              cellDepths);
    const float d1 = sampleSurfaceVertexDepth(tile + glm::ivec2{1, 0},
                                              build.minTile,
                                              cellWidth,
                                              cellHeight,
                                              cellDepths);
    const float d2 = sampleSurfaceVertexDepth(tile + glm::ivec2{1, 1},
                                              build.minTile,
                                              cellWidth,
                                              cellHeight,
                                              cellDepths);
    const float d3 = sampleSurfaceVertexDepth(tile + glm::ivec2{0, 1},
                                              build.minTile,
                                              cellWidth,
                                              cellHeight,
                                              cellDepths);

    const uint32_t i0 = addSurfaceVertex(
        context, tile + glm::ivec2{0, 0}, cell.elevation, d0, command);
    const uint32_t i1 = addSurfaceVertex(
        context, tile + glm::ivec2{1, 0}, cell.elevation, d1, command);
    const uint32_t i2 = addSurfaceVertex(
        context, tile + glm::ivec2{1, 1}, cell.elevation, d2, command);
    const uint32_t i3 = addSurfaceVertex(
        context, tile + glm::ivec2{0, 1}, cell.elevation, d3, command);

    command.indices.push_back(i0);
    command.indices.push_back(i1);
    command.indices.push_back(i2);

    command.indices.push_back(i0);
    command.indices.push_back(i2);
    command.indices.push_back(i3);
  }
}

uint32_t
IsometricWaterSystem::addSurfaceVertex(const IsometricRenderContext& context,
                                       const glm::ivec2& gridPoint,
                                       int waterElevation,
                                       float depth,
                                       SurfaceCommand& command) const
{
  const float depthFactor = std::clamp(depth / 6.0f, 0.0f, 1.0f);

  const SDL_Color shallowColor{55, 155, 210, 70};
  const SDL_Color deepColor = sfs::Colors::Navy.toSDL();

  const glm::vec4 shallow{
      shallowColor.r / 255.0f,
      shallowColor.g / 255.0f,
      shallowColor.b / 255.0f,
      shallowColor.a / 255.0f,
  };

  const glm::vec4 deep{
      deepColor.r / 255.0f,
      deepColor.g / 255.0f,
      deepColor.b / 255.0f,
      deepColor.a / 255.0f,
  };

  const glm::vec2 worldPosition{
      static_cast<float>(gridPoint.x),
      static_cast<float>(gridPoint.y),
  };

  SurfaceVertex vertex;
  vertex.worldPosition = worldPosition;
  vertex.position =
      context.worldToScreen(worldPosition, static_cast<float>(waterElevation));
  vertex.color = glm::mix(shallow, deep, depthFactor);
  vertex.uv = worldPosition;
  vertex.params = glm::vec4{
      depth,
      depthFactor,
      static_cast<float>(waterElevation),
      0.0f,
  };

  const uint32_t index = static_cast<uint32_t>(command.vertices.size());
  command.vertices.push_back(vertex);
  return index;
}

float IsometricWaterSystem::sampleSurfaceVertexDepth(
    const glm::ivec2& gridPoint,
    const glm::ivec2& minTile,
    int cellWidth,
    int cellHeight,
    const std::vector<float>& cellDepths) const
{
  float depthSum = 0.0f;
  float weightSum = 0.0f;

  for (int oy = -1; oy <= 0; oy++)
  {
    for (int ox = -1; ox <= 0; ox++)
    {
      const glm::ivec2 tile = gridPoint + glm::ivec2{ox, oy};

      const int x = tile.x - minTile.x;
      const int y = tile.y - minTile.y;

      if (x < 0 || y < 0 || x >= cellWidth || y >= cellHeight)
        continue;

      const float depth = cellDepths[static_cast<size_t>(y * cellWidth + x)];

      if (depth < 0.0f)
        continue;

      depthSum += depth;
      weightSum += 1.0f;
    }
  }

  return weightSum > 0.0f ? depthSum / weightSum : 0.0f;
}

} // namespace sfs
