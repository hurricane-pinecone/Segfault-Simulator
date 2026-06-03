#pragma once

#include "engine/ecs/registry.h" // IWYU pragma: keep -- registry->view<T...>()
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/isometricRenderContext.h"
#include "engine/rendering/renderProvider.h"

namespace sfs
{

struct WaterCell
{
  glm::ivec2 tile{};
  int elevation = 0;
  int terrainElevation;
  float depth = 0.0f;
};

struct WaterSurfaceBuild
{
  std::vector<WaterCell> cells;
  glm::ivec2 minTile{};
  glm::ivec2 maxTile{};
};

/**
 * Builds the water surface mesh from WaterTileComponent entities. A RenderProvider
 * owned by the render system: set the registry, then computeCommands() each frame
 * emits one SurfaceCommand per water tile cluster.
 */
class IsometricWaterProvider
    : public RenderProvider<IsometricRenderContext, SurfaceCommand>
{
public:
  void setRegistry(Registry* r) { registry = r; }

  void computeCommands(const IsometricRenderContext& context) override;

private:
  Registry* registry = nullptr;

  WaterSurfaceBuild
  collectWaterSurfaceBuild(const IsometricRenderContext& context) const;
  SurfaceCommand
  createWaterSurfaceCommand(const IsometricRenderContext& context,
                            const WaterCell& cell) const;

  uint32_t addSurfaceVertex(const IsometricRenderContext& context,
                            const glm::ivec2& gridPoint,
                            int waterElevation,
                            float depth,
                            float sortDepth,
                            SurfaceCommand& command) const;

  void buildSingleWaterTileMesh(const IsometricRenderContext& context,
                                const WaterSurfaceBuild& build,
                                const WaterCell& cell,
                                int cellWidth,
                                int cellHeight,
                                const std::vector<float>& cellDepths,
                                SurfaceCommand& command) const;

  float sampleSurfaceVertexDepth(const glm::ivec2& gridPoint,
                                 const glm::ivec2& minTile,
                                 int cellWidth,
                                 int cellHeight,
                                 const std::vector<float>& cellDepths) const;
};

} // namespace sfs
