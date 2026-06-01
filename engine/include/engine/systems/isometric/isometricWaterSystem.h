#pragma once

#include "engine/ecs/ecs.h" // IWYU pragma: keep
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

class IsometricWaterSystem
    : public System,
      public RenderProvider<IsometricRenderContext, SurfaceCommand>
{
public:
  void computeCommands(const IsometricRenderContext& context) override;

protected:
  void create() override;

private:
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
