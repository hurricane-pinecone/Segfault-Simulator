#include "engine/runtime/rendering/modules/isometricWater.h"
#include "engine/core/Color/Color.h"
#include "engine/core/components/elevationComponent.h"
#include "engine/core/components/surfaceEffect.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/components/waterTileComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep
#include "engine/core/util/profiling.h"
#include "engine/runtime/rendering/isometricRenderContext.h"
#include "engine/runtime/rendering/util/sdlColor.h"
#include "glm/glm/common.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sfs
{

void IsometricWater::computeCommands(const IsometricRenderContext& context)
{
  ZoneScopedN("Water: computeCommands");

  flush();

  WaterSurfaceBuild build = collectWaterSurfaceBuild(context);

  if (build.cells.empty())
    return;

  const int cellWidth = build.maxTile.x - build.minTile.x + 1;

  const int cellHeight = build.maxTile.y - build.minTile.y + 1;

  std::vector<float> cellDepths(
      static_cast<size_t>(cellWidth * cellHeight), -1.0f);
  // Surface height per tile (sentinel -1 = no water), averaged per grid vertex
  // so flowing tiles at different levels join into one continuous surface.
  std::vector<float> cellElevations(
      static_cast<size_t>(cellWidth * cellHeight), -1.0f);

  auto cellIndex = [&](const glm::ivec2& tile)
  {
    const int x = tile.x - build.minTile.x;
    const int y = tile.y - build.minTile.y;

    return static_cast<size_t>(y * cellWidth + x);
  };

  for (const WaterCell& cell : build.cells)
  {
    cellDepths[cellIndex(cell.tile)] = cell.depth;
    cellElevations[cellIndex(cell.tile)] = cell.elevation;
  }

  // All water merges into a single mesh / draw. The depth buffer resolves
  // occlusion against terrain per vertex (each vertex carries its tile's
  // sort-key), so water needs no per-depth split. Tiles are appended
  // back-to-front (ascending painter depth) so the translucent water blends in
  // the right order within the single mesh.
  const auto cellDepth = [](const WaterCell& cell)
  {
    return static_cast<float>(cell.tile.x + cell.tile.y + 1) +
           cell.elevation * 0.5f;
  };

  std::vector<const WaterCell*> sorted;
  sorted.reserve(build.cells.size());

  for (const WaterCell& cell : build.cells)
    if (cell.terrainElevation < cell.elevation)
      sorted.push_back(&cell);

  if (sorted.empty())
    return;

  std::sort(sorted.begin(),
            sorted.end(),
            [&](const WaterCell* a, const WaterCell* b)
            { return cellDepth(*a) < cellDepth(*b); });

  SurfaceCommand command = createWaterSurfaceCommand(context, *sorted.front());

  for (const WaterCell* cell : sorted)
    buildSingleWaterTileMesh(context,
                             build,
                             *cell,
                             cellWidth,
                             cellHeight,
                             cellDepths,
                             cellElevations,
                             command);

  if (!command.vertices.empty() && !command.indices.empty())
    m_commands.push_back(std::move(command));
}

SurfaceCommand
IsometricWater::createWaterSurfaceCommand(const IsometricRenderContext& context,
                                          const WaterCell& cell) const
{
  SurfaceCommand command;
  command.type = SurfaceEffect::Type::Water;

  // Water is translucent and renders in the Surfaces pass, after all opaque
  // geometry. The depth buffer occludes it against opaque terrain, so a
  // cliff/mountain in front hides the water behind it (correct shoreline
  // occlusion).
  //
  // Its depth uses the same world scale as sprites/tiles so the shared depth
  // buffer compares them correctly:
  //
  //   tile.x + 0.5 + tile.y + 0.5  ==  tile.x + tile.y + 1.0
  //
  // Subpass 2 keeps water just in front of an actor on the same tile (so water
  // blends over an actor in shallow water) via the subpass epsilon folded into
  // clip-z.
  command.order = RenderOrder{
      RenderPass::Surfaces,
      static_cast<float>(cell.tile.x + cell.tile.y + 1) + cell.elevation * 0.5f,
      2};
  command.ambient =
      context.ambientLighting ? context.ambientLighting->ambient : 1.0f;
  command.waveStrength = m_waveStrength;
  command.rippleStrength = m_rippleStrength;

  // The projection is affine, so a world delta maps to a constant clip-space
  // delta -- capture its basis vectors (the translation cancels) so the vertex
  // shader can project the Gerstner displacement.
  const glm::vec2 origin = context.worldToScreen({0.0f, 0.0f}, 0.0f);
  command.worldToClipX = context.worldToScreen({1.0f, 0.0f}, 0.0f) - origin;
  command.worldToClipY = context.worldToScreen({0.0f, 1.0f}, 0.0f) - origin;
  command.worldToClipE = context.worldToScreen({0.0f, 0.0f}, 1.0f) - origin;

  return command;
}

WaterSurfaceBuild IsometricWater::collectWaterSurfaceBuild(
    const IsometricRenderContext& context) const
{
  WaterSurfaceBuild build;

  // Voxel (or other) source: build cells from water columns instead of the ECS.
  if (m_waterSource)
  {
    std::vector<WaterColumn> columns;
    m_waterSource->collectWaterColumns(columns);
    build.cells.reserve(columns.size());

    bool hasBounds = false;
    for (const WaterColumn& column : columns)
    {
      build.cells.push_back(
          WaterCell{column.tile,
                    column.surfaceLevel,
                    column.floorLevel,
                    glm::max(0.0f, column.surfaceLevel - column.floorLevel)});

      if (!hasBounds)
      {
        build.minTile = column.tile;
        build.maxTile = column.tile;
        hasBounds = true;
      }
      else
      {
        build.minTile = glm::min(build.minTile, column.tile);
        build.maxTile = glm::max(build.maxTile, column.tile);
      }
    }

    return build;
  }

  const auto waterTiles =
      registry
          ->view<WaterTileComponent, TransformComponent, ElevationComponent>();
  build.cells.reserve(waterTiles.size());

  bool hasBounds = false;

  for (const auto& entity : waterTiles)
  {
    const auto& transform = entity.getComponent<TransformComponent>();
    const auto& water = entity.getComponent<WaterTileComponent>();
    const auto& elevation = entity.getComponent<ElevationComponent>();

    const glm::ivec2 tile = gridCellOf(transform.position);

    int terrainElevation = elevation.level;
    context.terrainElevationGrid.tryGet(tile, terrainElevation);

    build.cells.push_back(WaterCell{
        tile,
        static_cast<float>(water.elevation),
        static_cast<float>(terrainElevation),
        static_cast<float>(glm::max(0, water.elevation - terrainElevation))});

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

uint32_t IsometricWater::addSurfaceVertex(const IsometricRenderContext& context,
                                          const glm::vec2& worldPosition,
                                          float waterElevation,
                                          float depth,
                                          float sortDepth,
                                          SurfaceCommand& command) const
{
  const float depthFactor = glm::clamp(depth / 6.0f, 0.0f, 1.0f);

  const SDL_Color shallowColor = sfs::toSDL(sfs::Colors::Turqoise);
  const SDL_Color deepColor = sfs::toSDL(sfs::Colors::Navy);

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

  // World painter sort-key; assignClipDepth() remaps it to clip-space z.
  vertex.z = sortDepth;

  const uint32_t index = static_cast<uint32_t>(command.vertices.size());
  command.vertices.push_back(vertex);
  return index;
}

void IsometricWater::buildSingleWaterTileMesh(
    const IsometricRenderContext& context,
    const WaterSurfaceBuild& build,
    const WaterCell& cell,
    int cellWidth,
    int cellHeight,
    const std::vector<float>& cellDepths,
    const std::vector<float>& cellElevations,
    SurfaceCommand& command) const
{
  const glm::ivec2 tile = cell.tile;
  const glm::ivec2 corners[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

  // Both depth and surface height are averaged per shared grid vertex, so the
  // four corners join continuously with neighbouring tiles -- a tile one level
  // higher slopes into this one instead of leaving a vertical gap.
  float d[4];
  float e[4];
  for (int i = 0; i < 4; ++i)
  {
    const glm::ivec2 gridPoint = tile + corners[i];
    d[i] = sampleSurfaceVertex(
        gridPoint, build.minTile, cellWidth, cellHeight, cellDepths);
    e[i] = sampleSurfaceVertex(
        gridPoint, build.minTile, cellWidth, cellHeight, cellElevations);
  }

  // All four corners share the tile's painter sort-key so the merged mesh
  // occludes per tile exactly as the old per-depth water draws did.
  const float sortDepth =
      static_cast<float>(cell.tile.x + cell.tile.y + 1) + cell.elevation * 0.5f;

  // Bilinear interpolation across the tile (corners are 00,10,11,01).
  const auto bilerp = [](const float c[4], float u, float v)
  {
    const float top = c[0] + (c[1] - c[0]) * u;    // 00 -> 10
    const float bottom = c[3] + (c[2] - c[3]) * u; // 01 -> 11
    return top + (bottom - top) * v;
  };

  // Tessellate the tile into an N x N grid so the per-vertex wave displacement
  // (surface.vert) has the resolution to look choppy, not blocky. Elevation +
  // depth interpolate smoothly across the grid.
  constexpr int kTess = 4;
  const uint32_t base = static_cast<uint32_t>(command.vertices.size());

  for (int sy = 0; sy <= kTess; ++sy)
    for (int sx = 0; sx <= kTess; ++sx)
    {
      const float u = static_cast<float>(sx) / kTess;
      const float v = static_cast<float>(sy) / kTess;
      const glm::vec2 world{
          static_cast<float>(tile.x) + u, static_cast<float>(tile.y) + v};
      addSurfaceVertex(
          context, world, bilerp(e, u, v), bilerp(d, u, v), sortDepth, command);
    }

  constexpr int stride = kTess + 1;
  for (int sy = 0; sy < kTess; ++sy)
    for (int sx = 0; sx < kTess; ++sx)
    {
      const uint32_t v00 = base + static_cast<uint32_t>(sy * stride + sx);
      const uint32_t v10 = v00 + 1;
      const uint32_t v01 = v00 + stride;
      const uint32_t v11 = v01 + 1;
      command.indices.insert(
          command.indices.end(), {v00, v10, v11, v00, v11, v01});
    }
}

float IsometricWater::sampleSurfaceVertex(const glm::ivec2& gridPoint,
                                          const glm::ivec2& minTile,
                                          int cellWidth,
                                          int cellHeight,
                                          const std::vector<float>& field) const
{
  float sum = 0.0f;
  float weight = 0.0f;

  for (int oy = -1; oy <= 0; oy++)
  {
    for (int ox = -1; ox <= 0; ox++)
    {
      const glm::ivec2 tile = gridPoint + glm::ivec2{ox, oy};

      const int x = tile.x - minTile.x;
      const int y = tile.y - minTile.y;

      if (x < 0 || y < 0 || x >= cellWidth || y >= cellHeight)
        continue;

      const float value = field[static_cast<size_t>(y * cellWidth + x)];

      if (value < 0.0f) // sentinel: no water in this tile
        continue;

      sum += value;
      weight += 1.0f;
    }
  }

  return weight > 0.0f ? sum / weight : 0.0f;
}

} // namespace sfs
